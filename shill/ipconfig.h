// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SHILL_IPCONFIG_
#define SHILL_IPCONFIG_

#include <string>
#include <vector>

#include <base/callback_old.h>
#include <base/memory/ref_counted.h>
#include <base/memory/scoped_ptr.h>
#include <gtest/gtest_prod.h>  // for FRIEND_TEST

#include "shill/ip_address.h"
#include "shill/property_store.h"
#include "shill/refptr_types.h"

namespace shill {
class ControlInterface;
class Error;
class IPConfigAdaptorInterface;

// IPConfig superclass. Individual IP configuration types will inherit from this
// class.
class IPConfig : public base::RefCounted<IPConfig> {
 public:

  struct Properties {
    Properties() : address_family(IPAddress::kAddressFamilyUnknown),
                   subnet_cidr(0),
                   mtu(0) {}

    IPAddress::Family address_family;
    std::string address;
    int32 subnet_cidr;
    std::string broadcast_address;
    std::vector<std::string> dns_servers;
    std::string domain_name;
    std::vector<std::string> domain_search;
    std::string gateway;
    std::string method;
    std::string peer_address;
    int32 mtu;
  };

  IPConfig(ControlInterface *control_interface, const std::string &device_name);
  virtual ~IPConfig();

  const std::string &device_name() const { return device_name_; }

  std::string GetRpcIdentifier();

  // Registers a callback that's executed every time the configuration
  // properties change. Takes ownership of |callback|. Pass NULL to remove a
  // callback. The callback's first argument is a pointer to this IP
  // configuration instance allowing clients to more easily manage multiple IP
  // configurations. The callback's second argument is set to false if IP
  // configuration failed.
  void RegisterUpdateCallback(
      Callback2<const IPConfigRefPtr&, bool>::Type *callback);

  const Properties &properties() const { return properties_; }

  // Request, renew and release IP configuration. Return true on success, false
  // otherwise. The default implementation always returns false indicating a
  // failure.
  virtual bool RequestIP();
  virtual bool RenewIP();
  virtual bool ReleaseIP();

  PropertyStore *store() { return &store_; }

 protected:
  // Updates the IP configuration properties and notifies registered listeners
  // about the event. |success| is set to false if the IP configuration failed.
  void UpdateProperties(const Properties &properties, bool success);

  PropertyStore store_;

 private:
  friend class IPConfigAdaptorInterface;

  FRIEND_TEST(DeviceTest, AcquireDHCPConfig);
  FRIEND_TEST(DeviceTest, DestroyIPConfig);
  FRIEND_TEST(IPConfigTest, UpdateCallback);
  FRIEND_TEST(IPConfigTest, UpdateProperties);

  const std::string device_name_;
  scoped_ptr<IPConfigAdaptorInterface> adaptor_;
  Properties properties_;
  scoped_ptr<Callback2<const IPConfigRefPtr&, bool>::Type> update_callback_;

  DISALLOW_COPY_AND_ASSIGN(IPConfig);
};

}  // namespace shill

#endif  // SHILL_IPCONFIG_
