//
// Copyright (C) 2017 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#ifndef TRUNKS_POWER_MANAGER_H_
#define TRUNKS_POWER_MANAGER_H_

#include <memory>
#include <string>
#include <vector>

#include <base/memory/weak_ptr.h>
#include <power_manager/dbus-proxies.h>

#include "trunks/resource_manager.h"

namespace trunks {

// PowerManager handles suspend-resume events in the system.
//
class PowerManager {
 public:
  // The |resource_manager| will be notified of power events. This
  // class does not take ownership of |resource_manager|.
  explicit PowerManager(ResourceManager* resource_manager = nullptr)
    : resource_manager_(resource_manager) {}
  ~PowerManager() = default;

  void set_resource_manager(ResourceManager* resource_manager) {
    resource_manager_ = resource_manager;
  }

  void set_power_manager_proxy(
      org::chromium::PowerManagerProxyInterface* proxy) {
    proxy_ = proxy;
  }

  // Registers for power events on |bus|. The class doesn't take ownership
  // of |bus|.
  void Init(const scoped_refptr<dbus::Bus>& bus);

  // Tears down: unregisters SuspendDelay handlers.
  void TearDown();

 private:
  // Registers signal handlers for *SuspendImminent and SuspendDone.
  void RegisterSignalHandlers();

  // Called when powerd service becomes available on D-Bus.
  void OnServiceAvailable(bool available);
  // Called when the owner of powerd D-Bus interface changes.
  void OnOwnerChanged(const std::string& old_owner,
                      const std::string& new_owner);

  // Starts handling SuspendDelay.
  void Start();
  // Stops handling SuspendDelay.
  void Stop();

  // Called when SuspendDone signal is received.
  // |serialized_proto| contains the serialized signal payload.
  void OnResume(const std::vector<uint8_t>& serialized_proto);
  // Called when *SuspendImminent signal is received.
  // |serialized_proto| contains the serialized signal payload.
  void OnSuspend(const std::vector<uint8_t>& serialized_proto);

  // Called when SuspendDone signal handler is connected with
  // |interface_name| and |signal_name| identifying the signal and
  // |success| telling is the connection was successful.
  void OnResumeConnect(const std::string& interface_name,
                       const std::string& signal_name,
                       bool success);
  // Called when a signal handler is connected with
  // |interface_name| and |signal_name| identifying the signal and
  // |success| telling is the connection was successful.
  void OnSignalConnect(const std::string& interface_name,
                       const std::string& signal_name,
                       bool success);

  // Called if RegisterSuspendDelayRequest is successful.
  // |serialized_proto| contains the serialized reply payload.
  void OnRegisterSuspendDelaySuccess(
      const std::vector<uint8_t>& serialized_proto);
  // Called if a request is successful.
  // |serialized_proto| contains the serialized reply payload.
  void OnRequestSuccess(const std::string& message_name);
  // Called if a request failed. |message_name| identifies the request.
  // |error| contains information on the error.
  void OnRequestError(const std::string& message_name, brillo::Error* error);

  base::WeakPtr<PowerManager> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }
  base::WeakPtr<PowerManager> ThisForBind() {
    return GetWeakPtr();
  }

  // Whether SuspendDelay handler is registered.
  bool suspend_delay_registered_ = false;
  // Delay ID for SuspendDelay handler if registered.
  int32_t delay_id_ = 0;
  // Whether suspend handling is allowed (only if resume handler is
  // successfully registered, so that we have a way out of suspend).
  bool suspend_allowed_ = false;

  ResourceManager* resource_manager_;
  std::unique_ptr<org::chromium::PowerManagerProxy> dbus_proxy_;
  org::chromium::PowerManagerProxyInterface* proxy_ = nullptr;

  // Declared last so weak pointers are invalidated first on destruction.
  base::WeakPtrFactory<PowerManager> weak_factory_{this};
  DISALLOW_COPY_AND_ASSIGN(PowerManager);
};

}  // namespace trunks

#endif  // TRUNKS_POWER_MANAGER_H_
