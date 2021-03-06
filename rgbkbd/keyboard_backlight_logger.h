// Copyright 2022 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef RGBKBD_KEYBOARD_BACKLIGHT_LOGGER_H_
#define RGBKBD_KEYBOARD_BACKLIGHT_LOGGER_H_

#include <memory>

#include <stdint.h>
#include <string>

#include "base/files/file.h"
#include "rgbkbd/rgb_keyboard.h"

namespace rgbkbd {

class KeyboardBacklightLogger : public RgbKeyboard {
 public:
  KeyboardBacklightLogger();
  KeyboardBacklightLogger(const KeyboardBacklightLogger&) = delete;
  KeyboardBacklightLogger& operator=(const KeyboardBacklightLogger&) = delete;
  ~KeyboardBacklightLogger() override = default;

  bool SetKeyColor(uint32_t key, uint8_t r, uint8_t g, uint8_t b) override;
  bool SetAllKeyColors(uint8_t r, uint8_t g, uint8_t b) override;

  // Clears log.
  bool ResetLog();

 private:
  bool InitializeFile();
  bool WriteLogEntry(const std::string& log);

  std::unique_ptr<base::File> file_;
};

}  // namespace rgbkbd

#endif  // RGBKBD_KEYBOARD_BACKLIGHT_LOGGER_H_
