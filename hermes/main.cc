// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <base/logging.h>
#include <brillo/flag_helper.h>
#include <brillo/syslog_logging.h>

#include "hermes/daemon.h"

void Usage() {
  LOG(INFO) << "usage: ./hermes <smdp_hostname> <imei_number>";
}

int main(int argc, char** argv) {
  DEFINE_int32(log_level, 0,
               "Logging level - 0: LOG(INFO), 1: LOG(WARNING), 2: LOG(ERROR), "
               "-1: VLOG(1), -2: VLOG(2), ...");
  DEFINE_string(smdp_hostname, "", "SM-DP+ server hostname");
  DEFINE_string(imei, "", "IMEI number");
  DEFINE_string(matching_id, "", "Profile's matching ID number");
  brillo::FlagHelper::Init(argc, argv, "Chromium OS eSIM LPD Daemon");
  brillo::InitLog(brillo::kLogToSyslog | brillo::kLogToStderrIfTty);
  logging::SetMinLogLevel(FLAGS_log_level);

  hermes::Daemon daemon;
  return daemon.Run();
}
