// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tpm_manager/server/tpm_allowlist_impl.h"

#include <cstring>
#include <string>

#include <base/check.h>
#include <base/files/file_path.h>
#include <base/files/file_util.h>
#include <base/strings/string_util.h>
#include <libhwsec-foundation/tpm/tpm_version.h>

namespace {

#if USE_TPM_DYNAMIC

// The location of TPM DID & VID information.
constexpr char kTpmDidVidPath[] = "/sys/class/tpm/tpm0/did_vid";
// The location of system vendor information.
constexpr char kSysVendorPath[] = "/sys/class/dmi/id/sys_vendor";
// The location of product name information.
constexpr char kProductNamePath[] = "/sys/class/dmi/id/product_name";

struct TpmVidDid {
  uint16_t vendor_id;
  uint16_t device_id;
};

constexpr uint16_t kTpmVidAtmel = 0x1114;
constexpr uint16_t kTpmVidIbm = 0x1014;
constexpr uint16_t kTpmVidWinbond = 0x1050;
constexpr uint16_t kTpmVidIfx = 0x15D1;

constexpr TpmVidDid kTpm1DidVidAllowlist[] = {
    // Atmel TPM used in some Dell Latitudes.
    TpmVidDid{kTpmVidAtmel, 0x3204},
    // Emulated TPM provided by the swtpm program, used with QEMU.
    TpmVidDid{kTpmVidIbm, 0x1},
    // Enable TPM chip in Toshiba TCXWave 6140 tablet kiosk.
    TpmVidDid{kTpmVidWinbond, 0xFE},
    // The vendor is INFINEON, HP Elitebook 840 G1.
    TpmVidDid{kTpmVidIfx, 0xB},
    // The vendor is INFINEON, HP Elitebook 840 G2.
    TpmVidDid{kTpmVidIfx, 0x1A},
    // The vendor is INFINEON, HP Elitebook 840 G3.
    TpmVidDid{kTpmVidIfx, 0x1B},
};

constexpr TpmVidDid kTpm2DidVidAllowlist[] = {
    // Emulated TPM provided by the swtpm program, used with QEMU.
    TpmVidDid{kTpmVidIbm, 0x1},
};

struct DeviceModel {
  const char* sys_vendor;
  const char* product_name;
  TpmVidDid vid_did;
};

constexpr DeviceModel kTpm2ModelsAllowlist[] = {
    DeviceModel{"Dell Inc.", "Latitude 7490", TpmVidDid{kTpmVidWinbond, 0xFC}},
};

bool GetDidVid(uint16_t* did, uint16_t* vid) {
  base::FilePath file_path(kTpmDidVidPath);
  std::string did_vid_s;

  if (!base::ReadFileToString(file_path, &did_vid_s)) {
    return false;
  }

  uint32_t did_vid = 0;
  if (sscanf(did_vid_s.c_str(), "0x%X", &did_vid) != 1) {
    LOG(ERROR) << __func__ << ": Failed to parse TPM DID & VID: " << did_vid_s;
    return false;
  }

  *vid = did_vid & 0xFFFF;
  *did = did_vid >> 16;

  return true;
}

bool GetSysVendor(std::string* sys_vendor) {
  base::FilePath file_path(kSysVendorPath);
  std::string file_content;

  if (!base::ReadFileToString(file_path, &file_content)) {
    return false;
  }

  base::TrimWhitespaceASCII(file_content, base::TRIM_ALL, sys_vendor);
  return true;
}

bool GetProductName(std::string* product_name) {
  base::FilePath file_path(kProductNamePath);
  std::string file_content;

  if (!base::ReadFileToString(file_path, &file_content)) {
    return false;
  }

  base::TrimWhitespaceASCII(file_content, base::TRIM_ALL, product_name);
  return true;
}

#endif

}  // namespace

namespace tpm_manager {

TpmAllowlistImpl::TpmAllowlistImpl(TpmStatus* tpm_status)
    : tpm_status_(tpm_status) {
  CHECK(tpm_status_);
}

bool TpmAllowlistImpl::IsAllowed() {
#if !USE_TPM_DYNAMIC
  // Allow all kinds of TPM if we are not using runtime TPM selection.
  return true;
#else
  uint16_t device_id;
  uint16_t vendor_id;

  if (!GetDidVid(&device_id, &vendor_id)) {
    LOG(ERROR) << __func__ << ": Failed to get the TPM DID & VID.";
    return false;
  }

  TPM_SELECT_BEGIN;

  TPM2_SECTION({
    std::string sys_vendor;
    std::string product_name;

    if (!GetSysVendor(&sys_vendor)) {
      LOG(ERROR) << __func__ << ": Failed to get the system vendor.";
      return false;
    }
    if (!GetProductName(&product_name)) {
      LOG(ERROR) << __func__ << ": Failed to get the product name.";
      return false;
    }

    for (const DeviceModel& match : kTpm2ModelsAllowlist) {
      if (sys_vendor == match.sys_vendor &&
          product_name == match.product_name) {
        if (vendor_id == match.vid_did.vendor_id &&
            device_id == match.vid_did.device_id) {
          return true;
        }
      }
    }

    for (const TpmVidDid& match : kTpm2DidVidAllowlist) {
      if (device_id == match.device_id && vendor_id == match.vendor_id) {
        return true;
      }
    }

    LOG(INFO) << "Not allowed TPM2.0:";
    LOG(INFO) << "  System Vendor: " << sys_vendor;
    LOG(INFO) << "  Product Name: " << product_name;
    LOG(INFO) << "  TPM Vendor ID: " << std::hex << vendor_id;
    LOG(INFO) << "  TPM Device ID: " << std::hex << device_id;

    return false;
  });

  TPM1_SECTION({
    for (const TpmVidDid& match : kTpm1DidVidAllowlist) {
      if (device_id == match.device_id && vendor_id == match.vendor_id) {
        return true;
      }
    }

    LOG(INFO) << "Not allowed TPM1.2:";
    LOG(INFO) << "  TPM Vendor ID: " << std::hex << vendor_id;
    LOG(INFO) << "  TPM Device ID: " << std::hex << device_id;

    return false;
  });

  OTHER_TPM_SECTION({
    // We don't allow the other TPM cases.
    return false;
  });

  TPM_SELECT_END;
#endif
}

}  // namespace tpm_manager