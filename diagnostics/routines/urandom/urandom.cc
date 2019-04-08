// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "diagnostics/routines/urandom/urandom.h"

#include <utility>
#include <vector>

#include <base/logging.h>

#include "diagnostics/routines/diag_process_adapter_impl.h"
#include "diagnostics/routines/subproc_routine.h"

namespace diagnostics {

namespace {

constexpr char kUrandomExePath[] = "/usr/libexec/diagnostics/urandom";

}  // namespace

std::unique_ptr<DiagnosticRoutine> CreateUrandomRoutine(
    const grpc_api::UrandomRoutineParameters& parameters) {
  std::string time_delta_ms_value =
      std::to_string(base::TimeDelta::FromSeconds(parameters.length_seconds())
                         .InMilliseconds());
  return std::make_unique<SubprocRoutine>(
      base::CommandLine(std::vector<std::string>{
          kUrandomExePath, "--time_delta_ms=" + time_delta_ms_value,
          "--urandom_path=/dev/urandom"}),
      parameters.length_seconds());
}

}  // namespace diagnostics
