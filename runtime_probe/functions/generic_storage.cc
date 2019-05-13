// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "runtime_probe/functions/generic_storage.h"

#include <cstdint>
#include <utility>

#include <base/files/file_enumerator.h>
#include <base/files/file_util.h>
#include <base/json/json_reader.h>
#include <base/json/json_writer.h>
#include <base/strings/string_number_conversions.h>
#include <base/strings/string_split.h>
#include <brillo/strings/string_utils.h>
#include <chromeos/dbus/service_constants.h>
#include <dbus/bus.h>
#include <dbus/message.h>
#include <dbus/object_proxy.h>
#include <pcrecpp.h>

#include "runtime_probe/utils/file_utils.h"

namespace runtime_probe {
namespace {
constexpr auto kStorageDirPath("/sys/class/block/");
constexpr auto kReadFileMaxSize = 1024;
// Hard-coded logical block size as a fallback option when the authenticated
// source in sysfs is not available.
constexpr auto kDefaultBytesPerSector = 512;

// DBus related constant to issue dbus call to debugd
constexpr auto kDebugdMmcMethodName = "Mmc";
constexpr auto kDebugdMmcOption = "extcsd_read";
constexpr auto kDebugdMmcDefaultTimeout = 10 * 1000;  // in ms

const std::vector<std::string> kAtaFields{"vendor", "model"};
const std::vector<std::string> kEmmcFields{"name", "oemid", "manfid", "serial"};
// Attributes in optional fields:
// prv: SD and MMCv4 only
// hwrev: SD and MMCv1 only
const std::vector<std::string> kEmmcOptionalFields{"prv", "hwrev"};
const std::vector<std::string> kNvmeFields{"vendor", "device", "class"};

// Note that to be backward portable with old protocol buffer we use empty
// prefix for Emmc field
constexpr auto kEmmcType = "MMC";
constexpr auto kEmmcPrefix = "";
constexpr auto kAtaType = "ATA";
constexpr auto kAtaPrefix = "ata_";
constexpr auto kNvmeType = "NVMe";
constexpr auto kNvmePrefix = "pci_";

// Check if the string represented by |input_string| is printable
bool IsPrintable(const std::string& input_string) {
  for (const auto& cha : input_string) {
    if (!isprint(cha))
      return false;
  }
  return true;
}

// Return the formatted string "%s (%s)" % |v|, |v_decode|
std::string VersionFormattedString(const std::string& v,
                                   const std::string& v_decode) {
  return v + " (" + v_decode + ")";
}

// Append the given |prefix| to each key in the |dict_value|
void PrependToDVKey(base::DictionaryValue* dict_value,
                    const std::string& prefix) {
  if (prefix.empty())
    return;
  std::vector<std::string> original_keys;
  for (base::DictionaryValue::Iterator it(*dict_value); !it.IsAtEnd();
       it.Advance()) {
    original_keys.push_back(it.key());
  }
  for (const auto& key : original_keys) {
    std::unique_ptr<base::Value> tmp;
    dict_value->Remove(key, &tmp);
    dict_value->SetString(prefix + key, tmp->GetString());
  }
}

// Get the logical block size of the storage given the |node_path|
int32_t GetStorageLogicalBlockSize(const base::FilePath& node_path) {
  std::string block_size_str;
  if (!base::ReadFileToString(
          node_path.Append("queue").Append("logical_block_size"),
          &block_size_str)) {
    LOG(WARNING) << "The storage driver does not specify its logical block "
                    "size in sysfs. Use default value instead.";
    return kDefaultBytesPerSector;
  }
  base::TrimWhitespaceASCII(block_size_str, base::TrimPositions::TRIM_ALL,
                            &block_size_str);
  int32_t logical_block_size;
  if (!base::StringToInt(block_size_str, &logical_block_size)) {
    LOG(WARNING) << "Failed to convert retrieved block size to integer. Use "
                    "default value instead";
    return kDefaultBytesPerSector;
  }
  if (logical_block_size <= 0) {
    LOG(WARNING) << "The value of logical block size " << logical_block_size
                 << " seems errorneous. Use default value instead.";
    return kDefaultBytesPerSector;
  }
  return logical_block_size;
}

}  // namespace

bool GenericStorageFunction::GetOutputOfMmcExtcsd(
    const base::FilePath& node_path, std::string* output) const {
  VLOG(1) << "Issuing D-Bus call to debugd to retrieve eMMC 5.0 firmware info.";

  dbus::Bus::Options ops;
  ops.bus_type = dbus::Bus::SYSTEM;
  scoped_refptr<dbus::Bus> bus(new dbus::Bus(std::move(ops)));

  if (!bus->Connect()) {
    LOG(ERROR) << "Failed to connect to system D-Bus service.";
    return false;
  }
  dbus::ObjectProxy* object_proxy = bus->GetObjectProxy(
      debugd::kDebugdServiceName, dbus::ObjectPath(debugd::kDebugdServicePath));

  dbus::MethodCall method_call(debugd::kDebugdInterface, kDebugdMmcMethodName);
  dbus::MessageWriter writer(&method_call);

  writer.AppendString(kDebugdMmcOption);
  std::unique_ptr<dbus::Response> response =
      object_proxy->CallMethodAndBlock(&method_call, kDebugdMmcDefaultTimeout);
  if (!response) {
    LOG(ERROR) << "Failed to issue D-Bus mmc call to debugd.";
    return false;
  }

  dbus::MessageReader reader(response.get());
  if (!reader.PopString(output)) {
    LOG(ERROR) << "Failed to read reply from debugd.";
    return false;
  }
  return true;
}

std::vector<base::FilePath> GenericStorageFunction::GetFixedDevices() const {
  std::vector<base::FilePath> res{};
  const base::FilePath storage_dir_path(kStorageDirPath);
  base::FileEnumerator storage_dir_it(storage_dir_path, true,
                                      base::FileEnumerator::SHOW_SYM_LINKS |
                                          base::FileEnumerator::FILES |
                                          base::FileEnumerator::DIRECTORIES);

  while (true) {
    const auto storage_path = storage_dir_it.Next();
    if (storage_path.empty())
      break;
    // Only return non-removable devices
    const auto storage_removable_path = storage_path.Append("removable");
    std::string removable_res;
    if (!base::ReadFileToString(storage_removable_path, &removable_res)) {
      VLOG(2) << "Storage device " << storage_path.value()
              << " does not specify the removable property. May be a partition "
                 "of a storage device.";
      continue;
    }

    if (base::TrimWhitespaceASCII(removable_res, base::TrimPositions::TRIM_ALL)
            .as_string() != "0") {
      VLOG(2) << "Storage device " << storage_path.value() << " is removable.";
      continue;
    }

    // Skip Loobpack or dm-verity device
    if (base::StartsWith(storage_path.BaseName().value(), "loop",
                         base::CompareCase::SENSITIVE) ||
        base::StartsWith(storage_path.BaseName().value(), "dm-",
                         base::CompareCase::SENSITIVE))
      continue;

    res.push_back(storage_path);
  }

  return res;
}

std::string GenericStorageFunction::GetEMMC5FirmwareVersion(
    const base::FilePath& node_path) const {
  VLOG(2) << "Checking eMMC firmware version of "
          << node_path.BaseName().value();

  std::string ext_csd_res;

  if (!GetOutputOfMmcExtcsd(node_path, &ext_csd_res)) {
    LOG(WARNING) << "Fail to retrieve information from mmc extcsd for /dev/"
                 << node_path.BaseName().value();
    return std::string{""};
  }

  // The output of firmware version looks like hexdump of ASCII strings or
  // hexadecimal values, which depends on vendors.

  // Example of version "ABCDEFGH" (ASCII hexdump)
  // [FIRMWARE_VERSION[261]]: 0x48
  // [FIRMWARE_VERSION[260]]: 0x47
  // [FIRMWARE_VERSION[259]]: 0x46
  // [FIRMWARE_VERSION[258]]: 0x45
  // [FIRMWARE_VERSION[257]]: 0x44
  // [FIRMWARE_VERSION[256]]: 0x43
  // [FIRMWARE_VERSION[255]]: 0x42
  // [FIRMWARE_VERSION[254]]: 0x41

  // Example of version 3 (hexadecimal values hexdump)
  // [FIRMWARE_VERSION[261]]: 0x00
  // [FIRMWARE_VERSION[260]]: 0x00
  // [FIRMWARE_VERSION[259]]: 0x00
  // [FIRMWARE_VERSION[258]]: 0x00
  // [FIRMWARE_VERSION[257]]: 0x00
  // [FIRMWARE_VERSION[256]]: 0x00
  // [FIRMWARE_VERSION[255]]: 0x00
  // [FIRMWARE_VERSION[254]]: 0x03

  pcrecpp::RE re(R"(^\[FIRMWARE_VERSION\[\d+\]\]: (.*)$)",
                 pcrecpp::RE_Options());
  // version list stores each byte as the format "ff" (two hex digits)
  // raw version list stores each byte as an int

  const auto ext_csd_lines = base::SplitString(
      ext_csd_res, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);

  std::vector<std::string> hex_version_components;
  std::string char_version{""};

  // The memory snapshots of version output from mmc are in reverse order
  for (auto it = ext_csd_lines.rbegin(); it != ext_csd_lines.rend(); it++) {
    std::string cur_version_str;
    if (!re.PartialMatch(*it, &cur_version_str))
      continue;

    // 0xff => ff
    const auto cur_version_component =
        cur_version_str.substr(2, std::string::npos);

    hex_version_components.push_back(cur_version_component);

    int cur_version_char;
    if (!base::HexStringToInt(cur_version_str, &cur_version_char)) {
      LOG(ERROR) << "Failed to convert one byte hex representation "
                 << cur_version_str << " to char.";
      return std::string{""};
    }
    char_version += static_cast<char>(cur_version_char);
  }

  const auto hex_version = brillo::string_utils::JoinRange(
      "", hex_version_components.begin(), hex_version_components.end());
  VLOG(2) << "eMMC 5.0 firmware version is " << hex_version;
  // Convert each int in raw_version_list to char and concat them
  if (IsPrintable(char_version)) {
    return VersionFormattedString(hex_version, char_version);

  } else {
    // Represent the version in the little endian format
    const std::string hex_version_le = brillo::string_utils::JoinRange(
        "", hex_version_components.rbegin(), hex_version_components.rend());
    uint64_t version_decode_le;
    if (!base::HexStringToUInt64(hex_version_le, &version_decode_le)) {
      LOG(ERROR) << "Failed to convert " << hex_version_le
                 << " to 64-bit unsigned integer";
      return std::string{""};
    }
    return VersionFormattedString(hex_version,
                                  std::to_string(version_decode_le));
  }
}

GenericStorageFunction::DataType GenericStorageFunction::Eval() const {
  DataType result{};
  std::string json_output;
  if (!InvokeHelper(&json_output)) {
    LOG(ERROR) << "Failed to invoke helper to retrieve storage information.";
    return result;
  }
  const auto storage_results =
      base::ListValue::From(base::JSONReader::Read(json_output));
  for (int i = 0; i < storage_results->GetSize(); ++i) {
    base::DictionaryValue* storage_res;
    storage_results->GetDictionary(i, &storage_res);

    // Get eMMC 5.0 firmaware version
    std::string storage_type;
    std::string node_path_str;
    if (!storage_res->GetString("type", &storage_type)) {
      LOG(ERROR) << "Type of storage is not set in runtime_probe helper";
    }
    if (!storage_res->GetString("path", &node_path_str)) {
      LOG(ERROR) << "Path to storage node is not set in runtime_probe helper";
    }
    if (storage_type == kEmmcType && !node_path_str.empty()) {
      const auto emmc5_fw_ver =
        GetEMMC5FirmwareVersion(base::FilePath(node_path_str));
      if (!emmc5_fw_ver.empty())
        storage_res->SetString("emmc5_fw_ver", emmc5_fw_ver);
    }

    result.push_back(std::move(*storage_res));
  }
  return result;
}

int GenericStorageFunction::EvalInHelper(std::string* output) const {
  const auto storage_nodes_path_list = GetFixedDevices();
  base::ListValue result;

  for (const auto& node_path : storage_nodes_path_list) {
    VLOG(2) << "Processing the node " << node_path.value();
    base::DictionaryValue node_res{};

    const auto dev_path = node_path.Append("device");
    // For NVMe device, "/<node_path>/device/device/.." is expected.
    const auto nvme_dev_path = dev_path.Append("device");

    // dev_path is the paraent directory of nvme_dev_path
    if (!base::PathExists(dev_path)) {
      VLOG(1) << "None of ATA, NVMe or eMMC fields exist on storage device "
              << node_path.value();
      continue;
    }

    // ATA, NVMe and eMMC are mutually exclusive indicators
    base::DictionaryValue ata_res = MapFilesToDict(dev_path, kAtaFields, {});
    base::DictionaryValue emmc_res =
        MapFilesToDict(dev_path, kEmmcFields, kEmmcOptionalFields);

    if (!emmc_res.empty()) {
      PrependToDVKey(&emmc_res, kEmmcPrefix);
      emmc_res.SetString("type", kEmmcType);
    }

    if (!ata_res.empty()) {
      PrependToDVKey(&ata_res, kAtaPrefix);
      ata_res.SetString("type", kAtaType);
    }

    node_res.MergeDictionary(&ata_res);
    node_res.MergeDictionary(&emmc_res);

    if (base::PathExists(nvme_dev_path)) {
      base::DictionaryValue nvme_res =
          MapFilesToDict(nvme_dev_path, kNvmeFields, {});
      if (!nvme_res.empty()) {
        PrependToDVKey(&nvme_res, kNvmePrefix);
        nvme_res.SetString("type", kNvmeType);
      }
      node_res.MergeDictionary(&nvme_res);
    }

    if (node_res.empty()) {
      VLOG(1) << "Cannot probe ATA, NVMe or eMMC fields on storage device "
              << node_path.value();
      continue;
    }

    // Report the absolute path we probe the reported info from
    node_res.SetString("path", node_path.value());

    // Size info
    const auto size_path = node_path.Append("size");
    std::string size_content;
    if (base::ReadFileToStringWithMaxSize(size_path, &size_content,
                                          kReadFileMaxSize)) {
      const auto sector_str = base::TrimWhitespaceASCII(
          size_content, base::TrimPositions::TRIM_ALL);
      node_res.SetString("sectors", sector_str);
      int64_t sector_int;
      if (!base::StringToInt64(sector_str, &sector_int)) {
        LOG(ERROR) << "Failed to parse recorded sector of" << node_path.value()
                   << " to integer!";
        node_res.SetString("size", "-1");
      } else {
        node_res.SetString(
            "size", base::Int64ToString(sector_int *
                                        GetStorageLogicalBlockSize(node_path)));
      }
    } else {
      VLOG(2) << "Storage device " << node_path.value()
              << " does not specify size";
      node_res.SetString("sectors", "-1");
      node_res.SetString("size", "-1");
    }

    result.Append(node_res.CreateDeepCopy());
  }

  if (!base::JSONWriter::Write(result, output)) {
    LOG(ERROR)
        << "Failed to serialize generic battery probed result to json string";
    return -1;
  }
  return 0;
}

}  // namespace runtime_probe
