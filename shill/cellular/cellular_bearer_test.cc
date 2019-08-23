// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shill/cellular/cellular_bearer.h"

#include <ModemManager/ModemManager.h>

#include <memory>

#include "shill/mock_control.h"
#include "shill/mock_dbus_properties_proxy.h"
#include "shill/testing.h"

using std::string;
using std::vector;
using testing::_;
using testing::ByMove;
using testing::Return;
using testing::ReturnNull;

namespace shill {

namespace {

const RpcIdentifier kBearerDBusPath =
    RpcIdentifier("/org/freedesktop/ModemManager/Bearer/0");
const char kBearerDBusService[] = "org.freedesktop.ModemManager";
const char kDataInterface[] = "/dev/ppp0";
const char kIPv4Address[] = "10.0.0.1";
const char kIPv4Gateway[] = "10.0.0.254";
const int kIPv4SubnetPrefix = 8;
const char* const kIPv4DNS[] = {"10.0.0.2", "8.8.4.4", "8.8.8.8"};
const char kIPv6Address[] = "0:0:0:0:0:ffff:a00:1";
const char kIPv6Gateway[] = "0:0:0:0:0:ffff:a00:fe";
const int kIPv6SubnetPrefix = 16;
const char* const kIPv6DNS[] = {"0:0:0:0:0:ffff:a00:fe",
                                "0:0:0:0:0:ffff:808:404",
                                "0:0:0:0:0:ffff:808:808"};

}  // namespace

class CellularBearerTest : public testing::Test {
 public:
  CellularBearerTest()
      : control_(new MockControl()),
        bearer_(control_.get(), kBearerDBusPath, kBearerDBusService) {}

 protected:
  void VerifyDefaultProperties() {
    EXPECT_EQ(kBearerDBusPath, bearer_.dbus_path());
    EXPECT_EQ(kBearerDBusService, bearer_.dbus_service());
    EXPECT_FALSE(bearer_.connected());
    EXPECT_EQ("", bearer_.data_interface());
    EXPECT_EQ(IPConfig::kMethodUnknown, bearer_.ipv4_config_method());
    EXPECT_EQ(nullptr, bearer_.ipv4_config_properties());
    EXPECT_EQ(IPConfig::kMethodUnknown, bearer_.ipv6_config_method());
    EXPECT_EQ(nullptr, bearer_.ipv6_config_properties());
  }

  static KeyValueStore ConstructIPv4ConfigProperties(
      MMBearerIpMethod ipconfig_method) {
    KeyValueStore ipconfig_properties;
    ipconfig_properties.SetUint("method", ipconfig_method);
    if (ipconfig_method == MM_BEARER_IP_METHOD_STATIC) {
      ipconfig_properties.SetString("address", kIPv4Address);
      ipconfig_properties.SetString("gateway", kIPv4Gateway);
      ipconfig_properties.SetUint("prefix", kIPv4SubnetPrefix);
      ipconfig_properties.SetString("dns1", kIPv4DNS[0]);
      ipconfig_properties.SetString("dns2", kIPv4DNS[1]);
      ipconfig_properties.SetString("dns3", kIPv4DNS[2]);
    }
    return ipconfig_properties;
  }

  static KeyValueStore ConstructIPv6ConfigProperties(
      MMBearerIpMethod ipconfig_method) {
    KeyValueStore ipconfig_properties;
    ipconfig_properties.SetUint("method", ipconfig_method);
    if (ipconfig_method == MM_BEARER_IP_METHOD_STATIC) {
      ipconfig_properties.SetString("address", kIPv6Address);
      ipconfig_properties.SetString("gateway", kIPv6Gateway);
      ipconfig_properties.SetUint("prefix", kIPv6SubnetPrefix);
      ipconfig_properties.SetString("dns1", kIPv6DNS[0]);
      ipconfig_properties.SetString("dns2", kIPv6DNS[1]);
      ipconfig_properties.SetString("dns3", kIPv6DNS[2]);
    }
    return ipconfig_properties;
  }

  static KeyValueStore ConstructBearerProperties(
      bool connected,
      const string& data_interface,
      MMBearerIpMethod ipv4_config_method,
      MMBearerIpMethod ipv6_config_method) {
    KeyValueStore properties;
    properties.SetBool(MM_BEARER_PROPERTY_CONNECTED, connected);
    properties.SetString(MM_BEARER_PROPERTY_INTERFACE, data_interface);

    properties.SetKeyValueStore(
        MM_BEARER_PROPERTY_IP4CONFIG,
        ConstructIPv4ConfigProperties(ipv4_config_method));
    properties.SetKeyValueStore(
        MM_BEARER_PROPERTY_IP6CONFIG,
        ConstructIPv6ConfigProperties(ipv6_config_method));
    return properties;
  }

  void VerifyStaticIPv4ConfigMethodAndProperties() {
    EXPECT_EQ(IPConfig::kMethodStatic, bearer_.ipv4_config_method());
    const IPConfig::Properties* ipv4_config_properties =
        bearer_.ipv4_config_properties();
    ASSERT_NE(nullptr, ipv4_config_properties);
    EXPECT_EQ(IPAddress::kFamilyIPv4, ipv4_config_properties->address_family);
    EXPECT_EQ(kIPv4Address, ipv4_config_properties->address);
    EXPECT_EQ(kIPv4Gateway, ipv4_config_properties->gateway);
    EXPECT_EQ(kIPv4SubnetPrefix, ipv4_config_properties->subnet_prefix);
    ASSERT_EQ(3, ipv4_config_properties->dns_servers.size());
    EXPECT_EQ(kIPv4DNS[0], ipv4_config_properties->dns_servers[0]);
    EXPECT_EQ(kIPv4DNS[1], ipv4_config_properties->dns_servers[1]);
    EXPECT_EQ(kIPv4DNS[2], ipv4_config_properties->dns_servers[2]);
  }

  void VerifyStaticIPv6ConfigMethodAndProperties() {
    EXPECT_EQ(IPConfig::kMethodStatic, bearer_.ipv6_config_method());
    const IPConfig::Properties* ipv6_config_properties =
        bearer_.ipv6_config_properties();
    ASSERT_NE(nullptr, ipv6_config_properties);
    EXPECT_EQ(IPAddress::kFamilyIPv6, ipv6_config_properties->address_family);
    EXPECT_EQ(kIPv6Address, ipv6_config_properties->address);
    EXPECT_EQ(kIPv6Gateway, ipv6_config_properties->gateway);
    EXPECT_EQ(kIPv6SubnetPrefix, ipv6_config_properties->subnet_prefix);
    ASSERT_EQ(3, ipv6_config_properties->dns_servers.size());
    EXPECT_EQ(kIPv6DNS[0], ipv6_config_properties->dns_servers[0]);
    EXPECT_EQ(kIPv6DNS[1], ipv6_config_properties->dns_servers[1]);
    EXPECT_EQ(kIPv6DNS[2], ipv6_config_properties->dns_servers[2]);
  }

  std::unique_ptr<MockControl> control_;
  CellularBearer bearer_;
};

TEST_F(CellularBearerTest, Constructor) {
  VerifyDefaultProperties();
}

TEST_F(CellularBearerTest, Init) {
  auto properties_proxy = std::make_unique<MockDBusPropertiesProxy>();
  EXPECT_CALL(*properties_proxy, set_properties_changed_callback(_)).Times(1);
  EXPECT_CALL(*properties_proxy, GetAll(MM_DBUS_INTERFACE_BEARER))
      .WillOnce(Return(ConstructBearerProperties(true, kDataInterface,
                                                 MM_BEARER_IP_METHOD_STATIC,
                                                 MM_BEARER_IP_METHOD_STATIC)));
  EXPECT_CALL(*control_,
              CreateDBusPropertiesProxy(kBearerDBusPath, kBearerDBusService))
      .WillOnce(Return(ByMove(std::move(properties_proxy))));

  bearer_.Init();
  EXPECT_TRUE(bearer_.connected());
  EXPECT_EQ(kDataInterface, bearer_.data_interface());
  VerifyStaticIPv4ConfigMethodAndProperties();
  VerifyStaticIPv6ConfigMethodAndProperties();
}

TEST_F(CellularBearerTest, InitAndCreateDBusPropertiesProxyFails) {
  EXPECT_CALL(*control_,
              CreateDBusPropertiesProxy(kBearerDBusPath, kBearerDBusService))
      .WillOnce(ReturnNull());
  bearer_.Init();
  VerifyDefaultProperties();
}

TEST_F(CellularBearerTest, OnPropertiesChanged) {
  KeyValueStore properties;

  // If interface is not MM_DBUS_INTERFACE_BEARER, no updates should be done.
  bearer_.OnPropertiesChanged("", properties, vector<string>());
  VerifyDefaultProperties();

  properties.SetBool(MM_BEARER_PROPERTY_CONNECTED, true);
  bearer_.OnPropertiesChanged("", properties, vector<string>());
  VerifyDefaultProperties();

  // Update 'interface' property.
  properties.Clear();
  properties.SetString(MM_BEARER_PROPERTY_INTERFACE, kDataInterface);
  bearer_.OnPropertiesChanged(MM_DBUS_INTERFACE_BEARER, properties,
                              vector<string>());
  EXPECT_EQ(kDataInterface, bearer_.data_interface());

  // Update 'connected' property.
  properties.Clear();
  properties.SetBool(MM_BEARER_PROPERTY_CONNECTED, true);
  bearer_.OnPropertiesChanged(MM_DBUS_INTERFACE_BEARER, properties,
                              vector<string>());
  EXPECT_TRUE(bearer_.connected());
  // 'interface' property remains unchanged.
  EXPECT_EQ(kDataInterface, bearer_.data_interface());

  // Update 'ip4config' property.
  properties.Clear();
  properties.SetKeyValueStore(
      MM_BEARER_PROPERTY_IP4CONFIG,
      ConstructIPv4ConfigProperties(MM_BEARER_IP_METHOD_UNKNOWN));
  bearer_.OnPropertiesChanged(MM_DBUS_INTERFACE_BEARER, properties,
                              vector<string>());
  EXPECT_EQ(IPConfig::kMethodUnknown, bearer_.ipv4_config_method());

  properties.Clear();
  properties.SetKeyValueStore(
      MM_BEARER_PROPERTY_IP4CONFIG,
      ConstructIPv4ConfigProperties(MM_BEARER_IP_METHOD_PPP));
  bearer_.OnPropertiesChanged(MM_DBUS_INTERFACE_BEARER, properties,
                              vector<string>());
  EXPECT_EQ(IPConfig::kMethodPPP, bearer_.ipv4_config_method());

  properties.Clear();
  properties.SetKeyValueStore(
      MM_BEARER_PROPERTY_IP4CONFIG,
      ConstructIPv4ConfigProperties(MM_BEARER_IP_METHOD_STATIC));
  bearer_.OnPropertiesChanged(MM_DBUS_INTERFACE_BEARER, properties,
                              vector<string>());
  EXPECT_EQ(IPConfig::kMethodStatic, bearer_.ipv4_config_method());
  VerifyStaticIPv4ConfigMethodAndProperties();

  properties.Clear();
  properties.SetKeyValueStore(
      MM_BEARER_PROPERTY_IP4CONFIG,
      ConstructIPv4ConfigProperties(MM_BEARER_IP_METHOD_DHCP));
  bearer_.OnPropertiesChanged(MM_DBUS_INTERFACE_BEARER, properties,
                              vector<string>());
  EXPECT_EQ(IPConfig::kMethodDHCP, bearer_.ipv4_config_method());

  // Update 'ip6config' property.
  properties.Clear();
  properties.SetKeyValueStore(
      MM_BEARER_PROPERTY_IP6CONFIG,
      ConstructIPv6ConfigProperties(MM_BEARER_IP_METHOD_UNKNOWN));
  bearer_.OnPropertiesChanged(MM_DBUS_INTERFACE_BEARER, properties,
                              vector<string>());
  EXPECT_EQ(IPConfig::kMethodUnknown, bearer_.ipv6_config_method());

  properties.Clear();
  properties.SetKeyValueStore(
      MM_BEARER_PROPERTY_IP6CONFIG,
      ConstructIPv6ConfigProperties(MM_BEARER_IP_METHOD_PPP));
  bearer_.OnPropertiesChanged(MM_DBUS_INTERFACE_BEARER, properties,
                              vector<string>());
  EXPECT_EQ(IPConfig::kMethodPPP, bearer_.ipv6_config_method());

  properties.Clear();
  properties.SetKeyValueStore(
      MM_BEARER_PROPERTY_IP6CONFIG,
      ConstructIPv6ConfigProperties(MM_BEARER_IP_METHOD_STATIC));
  bearer_.OnPropertiesChanged(MM_DBUS_INTERFACE_BEARER, properties,
                              vector<string>());
  EXPECT_EQ(IPConfig::kMethodStatic, bearer_.ipv6_config_method());
  VerifyStaticIPv6ConfigMethodAndProperties();

  properties.Clear();
  properties.SetKeyValueStore(
      MM_BEARER_PROPERTY_IP6CONFIG,
      ConstructIPv6ConfigProperties(MM_BEARER_IP_METHOD_DHCP));
  bearer_.OnPropertiesChanged(MM_DBUS_INTERFACE_BEARER, properties,
                              vector<string>());
  EXPECT_EQ(IPConfig::kMethodDHCP, bearer_.ipv6_config_method());
}

}  // namespace shill
