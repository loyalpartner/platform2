// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SHILL_VPN_WIREGUARD_DRIVER_H_
#define SHILL_VPN_WIREGUARD_DRIVER_H_

#include <string>
#include <vector>

#include <base/files/file_path.h>
#include <gtest/gtest_prod.h>  // for FRIEND_TEST

#include "shill/ipconfig.h"
#include "shill/vpn/vpn_driver.h"

namespace shill {

class WireGuardDriver : public VPNDriver {
 public:
  WireGuardDriver(Manager* manager, ProcessManager* process_manager);
  WireGuardDriver(const WireGuardDriver&) = delete;
  WireGuardDriver& operator=(const WireGuardDriver&) = delete;

  ~WireGuardDriver();

  // Inherited from VPNDriver. During ConnectAsync(), we will try to create the
  // tunnel in the kernel at first. If that fails, then we will try to let the
  // userspace program open the tunnel.
  base::TimeDelta ConnectAsync(EventHandler* event_handler) override;
  void Disconnect() override;
  void OnConnectTimeout() override;
  IPConfig::Properties GetIPProperties() const override;
  std::string GetProviderType() const override;

  // These functions (including GetProvider() below) are overridden for
  // implementing the "WireGuard.Peers" property in both property store (as an
  // array of dicts) and storage (as an an array of json-encoded strings), and
  // its value is kept in |peers_| in this class. A special property in a peer
  // is "PresharedKey": this property cannot be read via RPC, so we need some
  // special handling during writing. Specifically, in a RPC call for setting
  // "WireGuard.Peers", the preshared key of a peer will not be cleared if the
  // client does not specify a value for it (i.e., the incoming request does not
  // contain this key).
  void InitPropertyStore(PropertyStore* store) override;
  bool Load(const StoreInterface* storage,
            const std::string& storage_id) override;
  // Save() will also trigger the key-pair generation if the private key is
  // empty. Given that Save() will always be called after any property changes
  // by Manager::ConfigureService*(), this guarantees that there will always be
  // a valid key pair in the service.
  // TODO(b/177877860): May need to change this logic when hardware-backed keys
  // come, especially when the service is switching between these two key types.
  bool Save(StoreInterface* storage,
            const std::string& storage_id,
            bool save_credentials) override;

 protected:
  KeyValueStore GetProvider(Error* error) override;

 private:
  // Friend class for testing.
  friend class WireGuardDriverTestPeer;

  static const VPNDriver::Property kProperties[];

  void CreateKernelWireGuardInterface();

  void StartUserspaceWireGuardTunnel();

  // Spawns the userspace wireguard process, which will setup the tunnel
  // interface and do the data tunneling. WireGuardProcessExited() will be
  // invoked if that process exits unexpectedly.
  bool SpawnWireGuard();
  void WireGuardProcessExited(int exit_code);

  // Generates a config file that will be used by wireguard-tools from the
  // profile and write its content into a temporary file. Writes the path to the
  // temporary file into |config_file_|;
  bool GenerateConfigFile();

  // Configures the interface via wireguard-tools when the interface is ready.
  void ConfigureInterface(bool created_in_kernel,
                          const std::string& interface_name,
                          int interface_index);
  void OnConfigurationDone(int exit_code);

  // Fills in |ip_properties_| (especially, the address and routes fields)
  // according to the properties in the profile.
  bool PopulateIPProperties();

  // Calls Cleanup(), and if there is a service associated through
  // ConnectAsync(), notifies it of the failure.
  void FailService(Service::ConnectFailure failure,
                   const std::string& error_details);
  // Resets states and deallocate all resources.
  void Cleanup();

  bool UpdatePeers(const Stringmaps& new_peers, Error* error);
  void ClearPeers(Error* error);

  Stringmaps peers_;

  EventHandler* event_handler_;
  pid_t wireguard_pid_ = -1;
  int interface_index_ = -1;
  IPConfig::Properties ip_properties_;
  base::FilePath config_file_;

  // Indicates that whether we have an open wg interface in the kernel which is
  // created via DeviceInfo now.
  bool kernel_interface_open_ = false;

  // This variable is set in Load() and Save(), and only used to check whether
  // we need to re-calculate the public key in Save().
  std::string saved_private_key_;

  // The following two fields are constants. Makes them member variables for
  // testing.
  base::FilePath config_directory_;
  gid_t vpn_gid_;

  base::WeakPtrFactory<WireGuardDriver> weak_factory_{this};
};

}  // namespace shill

#endif  // SHILL_VPN_WIREGUARD_DRIVER_H_