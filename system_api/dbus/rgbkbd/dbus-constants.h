// Copyright 2022 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SYSTEM_API_DBUS_RGBKBD_DBUS_CONSTANTS_H_
#define SYSTEM_API_DBUS_RGBKBD_DBUS_CONSTANTS_H_

namespace rgbkbd {

const char kRgbkbdServicePath[] = "/org/chromium/Rgbkbd";
const char kRgbkbdServiceName[] = "org.chromium.Rgbkbd";

// Methods
const char kGetRgbKeyboardCapabilities[] = "GetRgbKeyboardCapabilities";
const char kSetCapsLockState[] = "SetCapsLockState";
const char kSetStaticBackgroundColor[] = "SetStaticBackgroundColor";
const char kSetRainbowMode[] = "SetRainbowMode";
const char kSetTestingMode[] = "SetTestingMode";

enum class RgbKeyboardCapabilities {
  kNone = 0,
  kIndividualKey = 1,
  kFiveZone = 2,
};

}  // namespace rgbkbd

#endif  // SYSTEM_API_DBUS_RGBKBD_DBUS_CONSTANTS_H_
