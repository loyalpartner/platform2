// Copyright 2016 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Library to provide access to the Chrome OS master configuration

#include "chromeos-config/libcros_config/cros_config.h"
#include "chromeos-config/libcros_config/cros_config_impl.h"
#include "chromeos-config/libcros_config/identity.h"

#include <stdlib.h>

#include <iostream>
#include <sstream>
#include <string>

#include <base/files/file_path.h>
#include <base/files/file_util.h>
#include <base/logging.h>
#include <base/strings/string_split.h>
#include <base/strings/string_util.h>

namespace brillo {

bool CrosConfigInterface::IsLoggingEnabled() {
  static const char* logging_var = getenv("CROS_CONFIG_DEBUG");
  static bool enabled = logging_var && *logging_var;
  return enabled;
}

CrosConfigImpl::CrosConfigImpl() : inited_(false) {}

CrosConfigImpl::~CrosConfigImpl() {}

bool CrosConfigImpl::InitCheck() const {
  if (!inited_) {
    CROS_CONFIG_LOG(ERROR)
        << "Init*() must be called before accessing configuration";
    return false;
  }
  return true;
}

}  // namespace brillo
