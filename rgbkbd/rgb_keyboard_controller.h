// Copyright 2022 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef RGBKBD_RGB_KEYBOARD_CONTROLLER_H_
#define RGBKBD_RGB_KEYBOARD_CONTROLLER_H_

#include <cstdint>

#include "rgbkbd/rgb_keyboard.h"

namespace rgbkbd {

class RgbKeyboardController {
 public:
  RgbKeyboardController() = default;
  virtual ~RgbKeyboardController() = default;

  virtual uint32_t GetRgbKeyboardCapabilities() = 0;
  virtual void SetCapsLockState(bool enabled) = 0;
  virtual void SetStaticBackgroundColor(uint32_t r, uint32_t g, uint32_t b) = 0;
  virtual void SetRainbowMode() = 0;
  virtual void SetKeyboardClient(RgbKeyboard* keyboard) = 0;
};

}  // namespace rgbkbd

#endif  // RGBKBD_RGB_KEYBOARD_CONTROLLER_H_
