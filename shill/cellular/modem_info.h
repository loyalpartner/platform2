// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SHILL_CELLULAR_MODEM_INFO_H_
#define SHILL_CELLULAR_MODEM_INFO_H_

#include <memory>
#include <string>

#include <base/macros.h>
#include <gtest/gtest_prod.h>  // for FRIEND_TEST

namespace shill {

class ControlInterface;
class EventDispatcher;
class Manager;
class Metrics;
class ModemManager;
class PendingActivationStore;

// Manages modem managers.
class ModemInfo {
 public:
  ModemInfo(ControlInterface* control,
            EventDispatcher* dispatcher,
            Metrics* metrics,
            Manager* manager);
  virtual ~ModemInfo();

  virtual void Start();
  virtual void Stop();

  virtual void OnDeviceInfoAvailable(const std::string& link_name);

  ControlInterface* control_interface() const { return control_interface_; }
  EventDispatcher* dispatcher() const { return dispatcher_; }
  Metrics* metrics() const { return metrics_; }
  Manager* manager() const { return manager_; }
  PendingActivationStore* pending_activation_store() const {
    return pending_activation_store_.get();
  }

 protected:
  // Write accessors for unit-tests.
  void set_control_interface(ControlInterface* control) {
    control_interface_ = control;
  }
  void set_event_dispatcher(EventDispatcher* dispatcher) {
    dispatcher_ = dispatcher;
  }
  void set_metrics(Metrics* metrics) { metrics_ = metrics; }
  void set_manager(Manager* manager) { manager_ = manager; }
  void set_pending_activation_store(
      PendingActivationStore* pending_activation_store);

 private:
  std::unique_ptr<ModemManager> modem_manager_;
  ControlInterface* control_interface_;
  EventDispatcher* dispatcher_;
  Metrics* metrics_;
  Manager* manager_;

  // Post-payment activation state of the modem.
  std::unique_ptr<PendingActivationStore> pending_activation_store_;

  DISALLOW_COPY_AND_ASSIGN(ModemInfo);
};

}  // namespace shill

#endif  // SHILL_CELLULAR_MODEM_INFO_H_
