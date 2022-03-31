// Copyright 2022 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "vtpm/commands/null_command.h"

#include <string>
#include <utility>

#include <base/callback.h>

namespace vtpm {

void NullCommand::Run(const std::string& command,
                      CommandResponseCallback callback) {
  std::move(callback).Run("");
}

}  // namespace vtpm
