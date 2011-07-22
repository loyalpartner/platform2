// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shill/device.h"

#include <map>
#include <string>
#include <vector>

#include <dbus-c++/dbus.h>
#include <chromeos/dbus/service_constants.h>
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "shill/dbus_adaptor.h"
#include "shill/dhcp_provider.h"
#include "shill/manager.h"
#include "shill/mock_adaptors.h"
#include "shill/mock_control.h"
#include "shill/mock_device.h"
#include "shill/mock_glib.h"
#include "shill/mock_store.h"
#include "shill/property_store_unittest.h"
#include "shill/shill_event.h"

using std::map;
using std::string;
using std::vector;
using ::testing::_;
using ::testing::AtLeast;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::Test;

namespace shill {

namespace {
const char kDeviceName[] = "testdevice";
}  // namespace {}

class DeviceTest : public PropertyStoreTest {
 public:
  DeviceTest()
      : device_(new Device(&control_interface_, NULL, NULL, kDeviceName, 0)) {
    DHCPProvider::GetInstance()->glib_ = &glib_;
    DHCPProvider::GetInstance()->control_interface_ = &control_interface_;
  }
  virtual ~DeviceTest() {}

 protected:
  MockGLib glib_;
  MockControl control_interface_;
  DeviceRefPtr device_;
};

TEST_F(DeviceTest, Contains) {
  EXPECT_TRUE(device_->store()->Contains(flimflam::kNameProperty));
  EXPECT_FALSE(device_->store()->Contains(""));
}

TEST_F(DeviceTest, GetProperties) {
  map<string, ::DBus::Variant> props;
  Error error(Error::kInvalidProperty, "");
  {
    ::DBus::Error dbus_error;
    bool expected = true;
    device_->store()->SetBoolProperty(flimflam::kPoweredProperty,
                                      expected,
                                      &error);
    DBusAdaptor::GetProperties(device_->store(), &props, &dbus_error);
    ASSERT_FALSE(props.find(flimflam::kPoweredProperty) == props.end());
    EXPECT_EQ(props[flimflam::kPoweredProperty].reader().get_bool(),
              expected);
  }
  {
    ::DBus::Error dbus_error;
    DBusAdaptor::GetProperties(device_->store(), &props, &dbus_error);
    ASSERT_FALSE(props.find(flimflam::kNameProperty) == props.end());
    EXPECT_EQ(props[flimflam::kNameProperty].reader().get_string(),
              string(kDeviceName));
  }
}

TEST_F(DeviceTest, Dispatch) {
  ::DBus::Error error;
  EXPECT_TRUE(DBusAdaptor::DispatchOnType(device_->store(),
                                          flimflam::kPoweredProperty,
                                          PropertyStoreTest::kBoolV,
                                          &error));

  // Ensure that an attempt to write a R/O property returns InvalidArgs error.
  EXPECT_FALSE(DBusAdaptor::DispatchOnType(device_->store(),
                                           flimflam::kAddressProperty,
                                           PropertyStoreTest::kStringV,
                                           &error));
  EXPECT_EQ(invalid_args_, error.name());
}

TEST_F(DeviceTest, TechnologyIs) {
  EXPECT_FALSE(device_->TechnologyIs(Device::kEthernet));
}

TEST_F(DeviceTest, DestroyIPConfig) {
  ASSERT_FALSE(device_->ipconfig_.get());
  device_->ipconfig_ = new IPConfig(&control_interface_, kDeviceName);
  device_->DestroyIPConfig();
  ASSERT_FALSE(device_->ipconfig_.get());
}

TEST_F(DeviceTest, DestroyIPConfigNULL) {
  ASSERT_FALSE(device_->ipconfig_.get());
  device_->DestroyIPConfig();
  ASSERT_FALSE(device_->ipconfig_.get());
}

TEST_F(DeviceTest, AcquireDHCPConfig) {
  device_->ipconfig_ = new IPConfig(&control_interface_, "randomname");
  EXPECT_CALL(glib_, SpawnAsync(_, _, _, _, _, _, _, _))
      .WillOnce(Return(false));
  EXPECT_FALSE(device_->AcquireDHCPConfig());
  ASSERT_TRUE(device_->ipconfig_.get());
  EXPECT_EQ(kDeviceName, device_->ipconfig_->device_name());
  EXPECT_TRUE(device_->ipconfig_->update_callback_.get());
}

TEST_F(DeviceTest, Load) {
  NiceMock<MockStore> storage;
  const string id = device_->GetStorageIdentifier();
  EXPECT_CALL(storage, ContainsGroup(id)).WillOnce(Return(true));
  EXPECT_CALL(storage, GetBool(id, _, _))
      .Times(AtLeast(1))
      .WillRepeatedly(Return(true));
  EXPECT_TRUE(device_->Load(&storage));
}

TEST_F(DeviceTest, Save) {
  NiceMock<MockStore> storage;
  const string id = device_->GetStorageIdentifier();
  device_->ipconfig_ = new IPConfig(&control_interface_, kDeviceName);
  EXPECT_CALL(storage, SetString(id, _, _))
      .Times(AtLeast(1))
      .WillRepeatedly(Return(true));
  EXPECT_CALL(storage, SetBool(id, _, _))
      .Times(AtLeast(1))
      .WillRepeatedly(Return(true));
  EXPECT_TRUE(device_->Save(&storage));
}

}  // namespace shill
