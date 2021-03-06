// Copyright 2022 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIBEC_RGB_KEYBOARD_COMMAND_H_
#define LIBEC_RGB_KEYBOARD_COMMAND_H_

#include <algorithm>
#include <vector>

#include <brillo/brillo_export.h>
#include "libec/ec_command.h"
#include "libec/rgb_keyboard_params.h"

namespace ec {

class BRILLO_EXPORT RgbkbdSetColorCommand
    : public EcCommand<rgb_keyboard::Params, EmptyParam> {
 public:
  // <start_key> is the first ID of the keys whose colors will be changed to the
  // colors specified by <color>.
  explicit RgbkbdSetColorCommand(uint8_t start_key = 0,
                                 const std::vector<struct rgb_s>& color = {})
      : EcCommand(EC_CMD_RGBKBD_SET_COLOR, 0) {
    Req()->req.start_key = start_key;
    Req()->req.length = color.size();
    std::copy(color.begin(), color.end(), Req()->color.begin());
    SetReqSize(sizeof(rgb_keyboard::Header) +
               Req()->color.size() * sizeof(Req()->color[0]));
  }
  ~RgbkbdSetColorCommand() override = default;
};

static_assert(!std::is_copy_constructible<RgbkbdSetColorCommand>::value,
              "EcCommands are not copyable by default");
static_assert(!std::is_copy_assignable<RgbkbdSetColorCommand>::value,
              "EcCommands are not copy-assignable by default");

}  // namespace ec

#endif  // LIBEC_RGB_KEYBOARD_COMMAND_H_
