// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mist/udev.h"

#include <libudev.h>

#include <base/format_macros.h>
#include <base/logging.h>
#include <base/strings/stringprintf.h>

#include "mist/udev_device.h"
#include "mist/udev_enumerate.h"
#include "mist/udev_monitor.h"

using base::StringPrintf;

namespace mist {

Udev::Udev() : udev_(nullptr) {}

Udev::~Udev() {
  if (udev_) {
    udev_unref(udev_);
    udev_ = nullptr;
  }
}

bool Udev::Initialize() {
  CHECK(!udev_);

  udev_ = udev_new();
  if (udev_)
    return true;

  VLOG(2) << "udev_new() returned nullptr.";
  return false;
}

// static
std::unique_ptr<UdevDevice> Udev::CreateDevice(udev_device* device) {
  CHECK(device);

  auto device_to_return = std::make_unique<UdevDevice>(device);
  CHECK(device_to_return);

  // UdevDevice increases the reference count of the udev_device struct by one.
  // Thus, decrease the reference count of the udev_device struct by one before
  // returning UdevDevice.
  udev_device_unref(device);

  return device_to_return;
}

std::unique_ptr<UdevDevice> Udev::CreateDeviceFromSysPath(
    const char* sys_path) {
  udev_device* device = udev_device_new_from_syspath(udev_, sys_path);
  if (device)
    return CreateDevice(device);

  VLOG(2) << StringPrintf(
      "udev_device_new_from_syspath"
      "(%p, \"%s\") returned nullptr.",
      udev_, sys_path);
  return nullptr;
}

std::unique_ptr<UdevDevice> Udev::CreateDeviceFromDeviceNumber(
    char type, dev_t device_number) {
  udev_device* device = udev_device_new_from_devnum(udev_, type, device_number);
  if (device)
    return CreateDevice(device);

  VLOG(2) << StringPrintf(
      "udev_device_new_from_devnum"
      "(%p, %d, %" PRIu64 ") returned nullptr.",
      udev_, type, device_number);
  return nullptr;
}

std::unique_ptr<UdevDevice> Udev::CreateDeviceFromSubsystemSysName(
    const char* subsystem, const char* sys_name) {
  udev_device* device =
      udev_device_new_from_subsystem_sysname(udev_, subsystem, sys_name);
  if (device)
    return CreateDevice(device);

  VLOG(2) << StringPrintf(
      "udev_device_new_from_subsystem_sysname"
      "(%p, \"%s\", \"%s\") returned nullptr.",
      udev_, subsystem, sys_name);
  return nullptr;
}

std::unique_ptr<UdevEnumerate> Udev::CreateEnumerate() {
  udev_enumerate* enumerate = udev_enumerate_new(udev_);
  if (enumerate) {
    auto enumerate_to_return = std::make_unique<UdevEnumerate>(enumerate);

    // UdevEnumerate increases the reference count of the udev_enumerate struct
    // by one. Thus, decrease the reference count of the udev_enumerate struct
    // by one before returning UdevEnumerate.
    udev_enumerate_unref(enumerate);

    return enumerate_to_return;
  }

  VLOG(2) << StringPrintf("udev_enumerate_new(%p) returned nullptr.", udev_);
  return nullptr;
}

std::unique_ptr<UdevMonitor> Udev::CreateMonitorFromNetlink(const char* name) {
  udev_monitor* monitor = udev_monitor_new_from_netlink(udev_, name);
  if (monitor) {
    auto monitor_to_return = std::make_unique<UdevMonitor>(monitor);

    // UdevMonitor increases the reference count of the udev_monitor struct by
    // one. Thus, decrease the reference count of the udev_monitor struct by one
    // before returning UdevMonitor.
    udev_monitor_unref(monitor);

    return monitor_to_return;
  }

  VLOG(2) << StringPrintf(
      "udev_monitor_new_from_netlink"
      "(%p, \"%s\") returned nullptr.",
      udev_, name);
  return nullptr;
}

}  // namespace mist
