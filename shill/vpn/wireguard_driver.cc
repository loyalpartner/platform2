// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shill/vpn/wireguard_driver.h"

#include <poll.h>
#include <string>
#include <utility>
#include <vector>

#include <base/base64.h>
#include <base/bind.h>
#include <base/callback_helpers.h>
#include <base/files/file_util.h>
#include <base/json/json_reader.h>
#include <base/json/json_writer.h>
#include <base/stl_util.h>
#include <base/strings/strcat.h>
#include <base/strings/stringprintf.h>
#include <base/strings/string_split.h>
#include <base/time/time.h>
#include <chromeos/dbus/service_constants.h>
#include <crypto/random.h>

#include "shill/logging.h"
#include "shill/manager.h"
#include "shill/process_manager.h"
#include "shill/property_accessor.h"
#include "shill/store_interface.h"

namespace shill {

namespace Logging {
static auto kModuleLogScope = ScopeLogger::kVPN;
static std::string ObjectID(const WireGuardDriver*) {
  return "(wireguard_driver)";
}
}  // namespace Logging

namespace {

const char kWireGuardPath[] = "/usr/sbin/wireguard";
const char kWireGuardToolsPath[] = "/usr/bin/wg";
const char kDefaultInterfaceName[] = "wg0";

// Directory where wireguard configuration files are exported. The owner of this
// directory is vpn:vpn, so both shill and wireguard client can access it.
const char kWireGuardConfigDir[] = "/run/wireguard";

// Timeout value for spawning the userspace wireguard process and configuring
// the interface via wireguard-tools.
constexpr base::TimeDelta kConnectTimeout = base::TimeDelta::FromSeconds(10);

// User and group we use to run wireguard binaries.
const char kVpnUser[] = "vpn";
const char kVpnGroup[] = "vpn";
constexpr gid_t kVpnGid = 20174;

constexpr int kWgKeyLength = 32;
constexpr int kWgBase64KeyLength = (((kWgKeyLength) + 2) / 3) * 4;

// Properties of a peer.
struct PeerProperty {
  // A name will be used in 1) D-Bus API, 2) profile storage, and 3) config file
  // passed to wireguard-tools.
  const char* const name;
  // Checked only before connecting. We allow a partially configured service
  // from crosh.
  const bool is_required;
};
constexpr PeerProperty kPeerProperties[] = {
    {kWireGuardPeerPublicKey, true},
    {kWireGuardPeerPresharedKey, false},
    {kWireGuardPeerEndpoint, true},
    {kWireGuardPeerAllowedIPs, true},
    {kWireGuardPeerPersistentKeepalive, false},
};

std::string GenerateBase64PrivateKey() {
  uint8_t key[kWgKeyLength];
  crypto::RandBytes(key, kWgKeyLength);
  return base::Base64Encode(base::span<uint8_t>(key, kWgKeyLength));
}

// Invokes wireguard-tools to calculates the public key based on the given
// private key. Returns an empty string on error. Note that the call to
// wireguard-tools is blocking but with a timeout (kPollTimeout below).
std::string CalculateBase64PublicKey(const std::string& base64_private_key,
                                     ProcessManager* process_manager) {
  constexpr auto kPollTimeout = base::TimeDelta::FromMilliseconds(200);

  int stdin_fd = -1;
  int stdout_fd = -1;
  pid_t pid = process_manager->StartProcessInMinijailWithPipes(
      FROM_HERE, base::FilePath(kWireGuardToolsPath), {"pubkey"},
      /*environment=*/{}, kVpnUser, kVpnGroup, /*caps=*/0,
      /*inherit_supplementary_groups=*/true, /*close_nonstd_fds=*/true,
      /*exit_callback=*/base::DoNothing(),
      {.stdin_fd = &stdin_fd, .stdout_fd = &stdout_fd});
  if (pid == -1) {
    LOG(ERROR) << "Failed to run 'wireguard-tools pubkey'";
    return "";
  }

  base::ScopedFD scoped_stdin(stdin_fd);
  base::ScopedFD scoped_stdout(stdout_fd);

  if (!base::WriteFileDescriptor(scoped_stdin.get(), base64_private_key.c_str(),
                                 static_cast<int>(base64_private_key.size()))) {
    LOG(ERROR) << "Failed to send private key to wireguard-tools";
    process_manager->StopProcess(pid);
    return "";
  }
  scoped_stdin.reset();

  struct pollfd pollfds[] = {{
      .fd = scoped_stdout.get(),
      .events = POLLIN,
  }};
  int ret = poll(pollfds, 1, kPollTimeout.InMilliseconds());
  if (ret == -1) {
    PLOG(ERROR) << "poll() failed";
    process_manager->StopProcess(pid);
    return "";
  } else if (ret == 0) {
    LOG(ERROR) << "poll() timeout";
    process_manager->StopProcess(pid);
    return "";
  }

  char buf[kWgBase64KeyLength];
  ssize_t read_cnt =
      HANDLE_EINTR(read(scoped_stdout.get(), buf, size_t{kWgBase64KeyLength}));
  if (read_cnt == -1) {
    PLOG(ERROR) << "read() failed";
    process_manager->StopProcess(pid);
    return "";
  } else if (read_cnt != kWgBase64KeyLength) {
    LOG(ERROR) << "Failed to read enough chars for a public key. read_cnt="
               << read_cnt;
    process_manager->StopProcess(pid);
    return "";
  }

  return std::string{buf, std::string::size_type{kWgBase64KeyLength}};
}

}  // namespace

// static
const VPNDriver::Property WireGuardDriver::kProperties[] = {
    {kProviderHostProperty, 0},
    {kProviderTypeProperty, 0},

    // Properties for the interface. ListenPort is not here since we current
    // only support the "client mode".
    // TODO(b/177876632): Consider making this kCredential. Peer.PresharedKey
    // may need some similar handling.
    {kWireGuardPrivateKey, Property::kWriteOnly},
    // TODO(b/177877860): This field is for software-backed keys only. May need
    // to change this logic when hardware-backed keys come.
    {kWireGuardPublicKey, Property::kReadOnly},
    // Address for the wireguard interface.
    // TODO(b/177876632): Support IPv6 (multiple addresses).
    // TODO(b/177876632): Verify that putting other properties for the interface
    // (i.e., DNS and MTU) are in the StaticIPParameters works.
    {kWireGuardAddress, 0},
};

WireGuardDriver::WireGuardDriver(Manager* manager,
                                 ProcessManager* process_manager)
    : VPNDriver(manager, process_manager, kProperties, base::size(kProperties)),
      config_directory_(kWireGuardConfigDir),
      vpn_gid_(kVpnGid) {}

WireGuardDriver::~WireGuardDriver() {
  Cleanup();
}

base::TimeDelta WireGuardDriver::ConnectAsync(EventHandler* event_handler) {
  SLOG(this, 2) << __func__;
  event_handler_ = event_handler;
  // To make sure the connect procedure is executed asynchronously.
  dispatcher()->PostTask(
      FROM_HERE,
      base::BindOnce(&WireGuardDriver::CreateKernelWireGuardInterface,
                     weak_factory_.GetWeakPtr()));
  return kConnectTimeout;
}

void WireGuardDriver::Disconnect() {
  SLOG(this, 2) << __func__;
  Cleanup();
  event_handler_ = nullptr;
}

IPConfig::Properties WireGuardDriver::GetIPProperties() const {
  return ip_properties_;
}

std::string WireGuardDriver::GetProviderType() const {
  return kProviderWireGuard;
}

void WireGuardDriver::OnConnectTimeout() {
  FailService(Service::kFailureConnect, "Connect timeout");
}

void WireGuardDriver::InitPropertyStore(PropertyStore* store) {
  VPNDriver::InitPropertyStore(store);
  store->RegisterDerivedStringmaps(
      kWireGuardPeers,
      StringmapsAccessor(
          new CustomWriteOnlyAccessor<WireGuardDriver, Stringmaps>(
              this, &WireGuardDriver::UpdatePeers, &WireGuardDriver::ClearPeers,
              nullptr)));
}

KeyValueStore WireGuardDriver::GetProvider(Error* error) {
  KeyValueStore props = VPNDriver::GetProvider(error);
  Stringmaps copied_peers = peers_;
  for (auto& peer : copied_peers) {
    peer.erase(kWireGuardPeerPresharedKey);
  }
  props.Set<Stringmaps>(kWireGuardPeers, copied_peers);
  return props;
}

bool WireGuardDriver::Load(const StoreInterface* storage,
                           const std::string& storage_id) {
  if (!VPNDriver::Load(storage, storage_id)) {
    return false;
  }

  peers_.clear();

  std::vector<std::string> encoded_peers;
  if (!storage->GetStringList(storage_id, kWireGuardPeers, &encoded_peers)) {
    LOG(WARNING) << "Profile does not contain the " << kWireGuardPeers
                 << " property";
    return true;
  }

  for (const auto& peer_json : encoded_peers) {
    base::Optional<base::Value> val = base::JSONReader::Read(peer_json);
    if (!val || !val->is_dict()) {
      LOG(ERROR) << "Failed to parse a peer. Skipped it.";
      continue;
    }
    Stringmap peer;
    for (const auto& property : kPeerProperties) {
      const std::string key = property.name;
      const auto* value = val->FindStringKey(key);
      if (value != nullptr) {
        peer[key] = *value;
      } else {
        peer[key] = "";
      }
    }
    peers_.push_back(peer);
  }

  saved_private_key_ = args()->Lookup<std::string>(kWireGuardPrivateKey, "");

  return true;
}

bool WireGuardDriver::Save(StoreInterface* storage,
                           const std::string& storage_id,
                           bool save_credentials) {
  // Keys should be processed before calling VPNDriver::Save().
  auto private_key = args()->Lookup<std::string>(kWireGuardPrivateKey, "");
  if (private_key.empty()) {
    private_key = GenerateBase64PrivateKey();
    args()->Set<std::string>(kWireGuardPrivateKey, private_key);
  }
  if (private_key != saved_private_key_) {
    std::string public_key =
        CalculateBase64PublicKey(private_key, process_manager());
    if (public_key.empty()) {
      LOG(ERROR) << "Failed to calculate public key in Save().";
      return false;
    }
    args()->Set<std::string>(kWireGuardPublicKey, public_key);
    saved_private_key_ = public_key;
  }

  // Handles peers.
  std::vector<std::string> encoded_peers;
  for (auto& peer : peers_) {
    base::Value root(base::Value::Type::DICTIONARY);
    for (const auto& property : kPeerProperties) {
      const auto& key = property.name;
      root.SetStringKey(key, peer[key]);
    }
    std::string peer_json;
    if (!base::JSONWriter::Write(root, &peer_json)) {
      LOG(ERROR) << "Failed to write a peer into json";
      return false;
    }
    encoded_peers.push_back(peer_json);
  }

  if (!storage->SetStringList(storage_id, kWireGuardPeers, encoded_peers)) {
    LOG(ERROR) << "Failed to write " << kWireGuardPeers
               << " property into profile";
    return false;
  }

  return VPNDriver::Save(storage, storage_id, save_credentials);
}

void WireGuardDriver::CreateKernelWireGuardInterface() {
  auto link_ready_callback = base::BindOnce(
      &WireGuardDriver::ConfigureInterface, weak_factory_.GetWeakPtr(),
      /*created_in_kernel=*/true);
  auto failure_callback =
      base::BindOnce(&WireGuardDriver::StartUserspaceWireGuardTunnel,
                     weak_factory_.GetWeakPtr());
  if (!manager()->device_info()->CreateWireGuardInterface(
          kDefaultInterfaceName, std::move(link_ready_callback),
          std::move(failure_callback))) {
    StartUserspaceWireGuardTunnel();
  }
}

void WireGuardDriver::StartUserspaceWireGuardTunnel() {
  LOG(INFO) << "Failed to create a wireguard interface in the kernel. Fallback "
               "to userspace tunnel.";

  // Claims the interface before the wireguard process creates it.
  // TODO(b/177876632): Actually when the tunnel interface is ready, it cannot
  // guarantee that the wireguard-tools can talk with the userspace wireguard
  // process now. We should also wait for another event that the UAPI socket
  // appears (which is a UNIX-domain socket created by the userspace wireguard
  // process at a fixed path: `/var/run/wireguard/wg0.sock`).
  manager()->device_info()->AddVirtualInterfaceReadyCallback(
      kDefaultInterfaceName,
      base::BindOnce(&WireGuardDriver::ConfigureInterface,
                     weak_factory_.GetWeakPtr(),
                     /*created_in_kernel=*/false));

  if (!SpawnWireGuard()) {
    FailService(Service::kFailureInternal, "Failed to spawn wireguard process");
  }
}

bool WireGuardDriver::SpawnWireGuard() {
  SLOG(this, 2) << __func__;

  // TODO(b/177876632): Change this part after we decide the userspace binary to
  // use. For wireguard-go, we need to change the way to invoke minijail; for
  // wireugard-rs, we need to add `--disable-drop-privileges` or change the
  // capmask.
  std::vector<std::string> args = {
      "--foreground",
      kDefaultInterfaceName,
  };
  constexpr uint64_t kCapMask = CAP_TO_MASK(CAP_NET_ADMIN);
  wireguard_pid_ = process_manager()->StartProcessInMinijail(
      FROM_HERE, base::FilePath(kWireGuardPath), args,
      /*environment=*/{}, kVpnUser, kVpnGroup, kCapMask,
      /*inherit_supplementary_groups=*/true, /*close_nonstd_fds=*/true,
      base::BindRepeating(&WireGuardDriver::WireGuardProcessExited,
                          weak_factory_.GetWeakPtr()));
  return wireguard_pid_ > -1;
}

void WireGuardDriver::WireGuardProcessExited(int exit_code) {
  wireguard_pid_ = -1;
  FailService(
      Service::kFailureInternal,
      base::StringPrintf("wireguard process exited unexpectedly with code=%d",
                         exit_code));
}

bool WireGuardDriver::GenerateConfigFile() {
  std::vector<std::string> lines;

  // [Interface] section
  lines.push_back("[Interface]");
  const std::string private_key =
      args()->Lookup<std::string>(kWireGuardPrivateKey, "");
  if (private_key.empty()) {
    LOG(ERROR) << "PrivateKey is required but is empty or not set.";
    return false;
  }
  lines.push_back(base::StrCat({"PrivateKey", "=", private_key}));
  // TODO(b/177876632): FwMark can be set here.

  lines.push_back("");

  // [Peer] sections
  for (auto& peer : peers_) {
    lines.push_back("[Peer]");
    for (const auto& property : kPeerProperties) {
      const std::string val = peer[property.name];
      if (!val.empty()) {
        lines.push_back(base::StrCat({property.name, "=", val}));
      } else if (property.is_required) {
        LOG(ERROR) << property.name
                   << " in a peer is required but is empty or not set.";
        return false;
      }
    }
    lines.push_back("");
  }

  // Writes |lines| into the file.
  if (!base::CreateTemporaryFileInDir(config_directory_, &config_file_)) {
    LOG(ERROR) << "Failed to create wireguard config file.";
    return false;
  }

  std::string contents = base::JoinString(lines, "\n");
  if (!base::WriteFile(config_file_, contents)) {
    LOG(ERROR) << "Failed to write wireguard config file";
    return false;
  }

  // Makes the config file group-readable and change its group to "vpn". Note
  // that the owner of a file may change the group of the file to any group of
  // which that owner is a member, so we can change the group to "vpn" here
  // since "shill" is a member of "vpn". Keeps the file as user-readable to make
  // it readable in unit tests.
  if (chmod(config_file_.value().c_str(), S_IRUSR | S_IRGRP) != 0) {
    PLOG(ERROR) << "Failed to make config file group-readable";
    return false;
  }
  if (chown(config_file_.value().c_str(), -1, vpn_gid_) != 0) {
    PLOG(ERROR) << "Failed to change gid of config file";
    return false;
  }

  return true;
}

void WireGuardDriver::ConfigureInterface(bool created_in_kernel,
                                         const std::string& interface_name,
                                         int interface_index) {
  LOG(INFO) << "WireGuard interface " << interface_name << " was created "
            << (created_in_kernel ? "in kernel" : "by userspace program")
            << ". Start configuration";
  kernel_interface_open_ = created_in_kernel;

  if (!event_handler_) {
    LOG(ERROR) << "Missing event_handler_";
    Cleanup();
    return;
  }

  interface_index_ = interface_index;

  if (!GenerateConfigFile()) {
    FailService(Service::kFailureInternal, "Failed to generate config file");
    return;
  }

  std::vector<std::string> args = {"setconf", kDefaultInterfaceName,
                                   config_file_.value()};
  constexpr uint64_t kCapMask = CAP_TO_MASK(CAP_NET_ADMIN);
  pid_t pid = process_manager()->StartProcessInMinijail(
      FROM_HERE, base::FilePath(kWireGuardToolsPath), args,
      /*environment=*/{}, kVpnUser, kVpnGroup, kCapMask, true, true,
      base::BindRepeating(&WireGuardDriver::OnConfigurationDone,
                          weak_factory_.GetWeakPtr()));
  if (pid == -1) {
    FailService(Service::kFailureInternal, "Failed to run `wg setconf`");
    return;
  }
}

void WireGuardDriver::OnConfigurationDone(int exit_code) {
  SLOG(this, 2) << __func__ << ": exit_code=" << exit_code;

  if (exit_code != 0) {
    FailService(
        Service::kFailureInternal,
        base::StringPrintf("Failed to run `wg setconf`, code=%d", exit_code));
    return;
  }

  if (!PopulateIPProperties()) {
    FailService(Service::kFailureInternal, "Failed to populate ip properties");
    return;
  }

  event_handler_->OnDriverConnected(kDefaultInterfaceName, interface_index_);
}

bool WireGuardDriver::PopulateIPProperties() {
  ip_properties_.default_route = false;

  const auto address =
      IPAddress(args()->Lookup<std::string>(kWireGuardAddress, ""));
  if (!address.IsValid()) {
    LOG(ERROR) << "WireGuardAddress property is not valid";
    return false;
  }
  ip_properties_.address_family = address.family();
  ip_properties_.address = address.ToString();

  // When we arrive here, the value of AllowedIPs has already been validated
  // by wireguard-tools. AllowedIPs is comma-separated list of CIDR-notation
  // addresses (e.g., "10.8.0.1/16,192.168.1.1/24").
  for (auto& peer : peers_) {
    std::string allowed_ips_str = peer[kWireGuardPeerAllowedIPs];
    std::vector<std::string> allowed_ip_list = base::SplitString(
        allowed_ips_str, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
    for (const auto& allowed_ip_str : allowed_ip_list) {
      IPAddress allowed_ip;
      // Currently only supports IPv4 addresses.
      allowed_ip.set_family(IPAddress::kFamilyIPv4);
      if (!allowed_ip.SetAddressAndPrefixFromString(allowed_ip_str)) {
        LOG(DFATAL) << "Invalid allowed ip: " << allowed_ip_str;
        return false;
      }
      // We don't need a gateway here, so use the "default" address as the
      // gateways, and then RoutingTable will skip RTA_GATEWAY when installing
      // this entry.
      ip_properties_.routes.push_back({allowed_ip.GetNetworkPart().ToString(),
                                       static_cast<int>(allowed_ip.prefix()),
                                       /*gateway=*/"0.0.0.0"});
    }
  }
  ip_properties_.method = kTypeVPN;
  return true;
}

void WireGuardDriver::FailService(Service::ConnectFailure failure,
                                  const std::string& error_details) {
  LOG(ERROR) << "Driver error: " << error_details;
  Cleanup();
  if (event_handler_) {
    event_handler_->OnDriverFailure(failure, error_details);
    event_handler_ = nullptr;
  }
}

void WireGuardDriver::Cleanup() {
  if (wireguard_pid_ != -1) {
    process_manager()->StopProcess(wireguard_pid_);
    wireguard_pid_ = -1;
  }
  if (kernel_interface_open_) {
    manager()->device_info()->DeleteInterface(interface_index_);
    kernel_interface_open_ = false;
  }
  interface_index_ = -1;
  ip_properties_ = {};
  if (!config_file_.empty()) {
    if (!base::DeleteFile(config_file_)) {
      LOG(ERROR) << "Failed to delete wireguard config file";
    }
    config_file_.clear();
  }
}

bool WireGuardDriver::UpdatePeers(const Stringmaps& new_peers, Error* error) {
  // If the preshared key of a peer in the new peers is unspecified (the caller
  // doesn't set that key), try to reset it to the old value.
  Stringmap pubkey_to_psk;
  for (auto& peer : peers_) {
    pubkey_to_psk[peer[kWireGuardPeerPublicKey]] =
        peer[kWireGuardPeerPresharedKey];
  }

  peers_ = new_peers;
  for (const auto& [pubkey, psk] : pubkey_to_psk) {
    for (auto& peer : peers_) {
      if (peer[kWireGuardPeerPublicKey] != pubkey ||
          peer.find(kWireGuardPeerPresharedKey) != peer.end()) {
        continue;
      }
      peer[kWireGuardPeerPresharedKey] = psk;
    }
  }

  return true;
}

void WireGuardDriver::ClearPeers(Error* error) {
  peers_.clear();
}

}  // namespace shill