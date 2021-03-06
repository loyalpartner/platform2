// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "debugd/src/perf_tool.h"

#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#include <map>
#include <utility>

#include <base/bind.h>
#include <base/check.h>
#include <base/check_op.h>
#include <base/logging.h>
#include <base/strings/stringprintf.h>
#include <base/time/time.h>

#include "debugd/src/error_utils.h"
#include "debugd/src/helpers/scheduler_configuration_utils.h"
#include "debugd/src/process_with_output.h"

namespace debugd {

namespace {

const char kUnsupportedPerfToolErrorName[] =
    "org.chromium.debugd.error.UnsupportedPerfTool";
const char kProcessErrorName[] = "org.chromium.debugd.error.RunProcess";
const char kStopProcessErrorName[] = "org.chromium.debugd.error.StopProcess";
const char kInvalidPerfArgumentErrorName[] =
    "org.chromium.debugd.error.InvalidPerfArgument";

const char kArgsError[] =
    "perf_args must begin with {\"perf\", \"record\"}, "
    " {\"perf\", \"stat\"}, or {\"perf\", \"mem\"}";

// Location of quipper on ChromeOS.
const char kQuipperLocation[] = "/usr/bin/quipper";

// Location of the file which contains the range of online CPU numbers.
const char kCpuTopologyLocation[] = "/sys/devices/system/cpu/online";
// Pattern of the directories that contain information about the idle state of
// a CPU on the system.
const char kCpuIdleStatePathPattern[] =
    "/sys/devices/system/cpu/cpu%s/cpuidle/state%d/disable";

// Location of the default ETM strobbing settings in configfs.
const char kStrobbingSettingPathPattern[] =
    "/sys/kernel/config/cs-syscfg/features/strobing/params/%s/value";
const int kStrobbingWindow = 512;
const int kStrobbingPeriod = 10000;

enum class OptionType {
  Boolean,  // Has no value.
  Value,    // Uses another argument.
};

// All quipper options and whether they are blocked in the debugd perf_tool.
const std::map<std::string, OptionType> kQuipperOptions = {
    {"--duration", OptionType::Value},
    // Blocked, quipper figures out the full path of perf on its own.
    // {"--perf_path", OptionType::Value},
    // Blocked, perf_tool always return via stdout.
    // {"--output_file", OptionType::Value},
    {"--run_inject", OptionType::Boolean},
    {"--inject_args", OptionType::Value},
};

// Returns one of the above enums given an vector of perf arguments, starting
// with "perf" itself in |args[0]|.
PerfSubcommand GetPerfSubcommandType(std::string command) {
  if (command == "record")
    return PERF_COMMAND_RECORD;
  if (command == "stat")
    return PERF_COMMAND_STAT;
  if (command == "mem")
    return PERF_COMMAND_MEM;
  return PERF_COMMAND_UNSUPPORTED;
}

void AddQuipperArguments(brillo::Process* process,
                         const uint32_t duration_secs,
                         const std::vector<std::string>& perf_args) {
  process->AddArg(kQuipperLocation);
  if (duration_secs > 0) {
    process->AddArg(base::StringPrintf("%u", duration_secs));
  }
  for (const auto& arg : perf_args) {
    process->AddArg(arg);
  }
}

}  // namespace

bool ValidateQuipperArguments(const std::vector<std::string>& qp_args,
                              PerfSubcommand& subcommand,
                              brillo::ErrorPtr* error) {
  for (auto args_iter = qp_args.begin(); args_iter != qp_args.end();
       ++args_iter) {
    if (*args_iter == "--") {
      ++args_iter;
      if (args_iter == qp_args.end()) {
        DEBUGD_ADD_ERROR(error, kUnsupportedPerfToolErrorName, kArgsError);
        return false;
      }

      subcommand = GetPerfSubcommandType(*args_iter);
      if (subcommand == PERF_COMMAND_UNSUPPORTED) {
        DEBUGD_ADD_ERROR(error, kUnsupportedPerfToolErrorName, kArgsError);
        return false;
      }

      return true;
    }

    const auto& it = kQuipperOptions.find(*args_iter);
    if (it == kQuipperOptions.end()) {
      DEBUGD_ADD_ERROR_FMT(error, kInvalidPerfArgumentErrorName,
                           "option %s is not allowed", args_iter->c_str());
      return false;
    }

    if (it->second == OptionType::Value) {
      ++args_iter;
      if (args_iter == qp_args.end()) {
        DEBUGD_ADD_ERROR_FMT(error, kInvalidPerfArgumentErrorName,
                             "option %s needs a following value",
                             args_iter->c_str());
        return false;
      }
    }
  }
  DEBUGD_ADD_ERROR(error, kUnsupportedPerfToolErrorName, kArgsError);
  return false;
}

PerfTool::PerfTool() {
  signal_handler_.Init();
  process_reaper_.Register(&signal_handler_);
  EtmStrobbingSettings();
}

bool PerfTool::DisableCpuIdleStates() {
  if (!cpuidle_states_.empty()) {
    LOG(ERROR) << "The cpuidle states are disabled already.";
    return false;
  }
  std::string cpu_range;
  if (!base::ReadFileToString(base::FilePath(kCpuTopologyLocation),
                              &cpu_range)) {
    PLOG(ERROR) << "File listing online CPU range missing.";
    return false;
  }

  std::vector<std::string> cpu_nums;
  if (!SchedulerConfigurationUtils::ParseCPUNumbers(cpu_range, &cpu_nums)) {
    PLOG(ERROR) << "Failed to parse CPU range: " << cpu_range << ".";
    return false;
  }

  for (const auto& cpu : cpu_nums) {
    for (int state = 0;; ++state) {
      const auto disable_file = base::FilePath(
          base::StringPrintf(kCpuIdleStatePathPattern, cpu.c_str(), state));
      if (!base::PathExists(disable_file))
        break;

      std::string disable_state;
      base::ReadFileToString(disable_file, &disable_state);
      cpuidle_states_.emplace(disable_file, disable_state);
      base::WriteFile(disable_file, "1");
    }
  }
  return true;
}

void PerfTool::RestoreCpuIdleStates() {
  for (const auto& [path, disable_state] : cpuidle_states_) {
    if (base::PathExists(path))
      base::WriteFile(path, disable_state);
  }
  cpuidle_states_.clear();
}

bool PerfTool::GetPerfOutputV2(const std::vector<std::string>& quipper_args,
                               bool disable_cpu_idle,
                               const base::ScopedFD& stdout_fd,
                               uint64_t* session_id,
                               brillo::ErrorPtr* error) {
  PerfSubcommand subcommand;
  if (!ValidateQuipperArguments(quipper_args, subcommand, error))
    return false;

  if (perf_running()) {
    // Do not run multiple sessions at the same time. Attempt to start another
    // profiler session using this method yields a DBus error. Note that
    // starting another session using GetPerfOutput() will still succeed.
    DEBUGD_ADD_ERROR(error, kProcessErrorName, "Existing perf tool running.");
    return false;
  }

  if (disable_cpu_idle) {
    if (!DisableCpuIdleStates()) {
      DEBUGD_ADD_ERROR(error, kProcessErrorName,
                       "Failed to disable CPU idle states");
      return false;
    }
  }

  DCHECK(!profiler_session_id_);

  auto quipper_process = std::make_unique<SandboxedProcess>();
  quipper_process->SandboxAs("root", "root");
  if (!quipper_process->Init()) {
    DEBUGD_ADD_ERROR(error, kProcessErrorName,
                     "Process initialization failure.");
    return false;
  }

  AddQuipperArguments(quipper_process.get(), 0, quipper_args);
  quipper_process->BindFd(stdout_fd.get(), 1);

  if (!quipper_process->Start()) {
    DEBUGD_ADD_ERROR(error, kProcessErrorName, "Process start failure.");
    return false;
  }
  quipper_process_ = std::move(quipper_process);
  DCHECK_GT(quipper_process_->pid(), 0);

  process_reaper_.WatchForChild(
      FROM_HERE, quipper_process_->pid(),
      base::BindOnce(&PerfTool::OnQuipperProcessExited,
                     base::Unretained(this)));

  // When GetPerfOutputV2() is used to run the perf tool, the user will read
  // from the read end of |stdout_fd| until the write end is closed.  At that
  // point, it may make another call to GetPerfOutputFd() and expect that will
  // start another perf run. |stdout_fd| will be closed when the last process
  // holding it exits, which is minijail0 in this case. However, the kernel
  // closes fds before signaling process exit. Therefore, it's possible for
  // |stdout_fd| to be closed and the user tries to run another
  // GetPerfOutputFd() before we're signaled of the process exit. To mitigate
  // this, hold on to a dup() of |stdout_fd| until we're signaled that the
  // process has exited. This guarantees that the caller can make a new
  // GetPerfOutputFd() call when it finishes reading the output.
  quipper_process_output_fd_.reset(dup(stdout_fd.get()));
  DCHECK(quipper_process_output_fd_.is_valid());

  // Generate an opaque, pseudo-unique, session ID using time and process ID.
  profiler_session_id_ = *session_id =
      static_cast<uint64_t>(base::Time::Now().ToTimeT()) << 32 |
      (quipper_process_->pid() & 0xffffffff);

  return true;
}

bool PerfTool::GetPerfOutput(uint32_t duration_secs,
                             const std::vector<std::string>& perf_args,
                             std::vector<uint8_t>* perf_data,
                             std::vector<uint8_t>* perf_stat,
                             int32_t* status,
                             brillo::ErrorPtr* error) {
  PerfSubcommand subcommand;
  if (duration_secs > 0) {  // legacy option style
    if (perf_args.size() < 2) {
      DEBUGD_ADD_ERROR(error, kUnsupportedPerfToolErrorName, kArgsError);
      return false;
    }
    subcommand = GetPerfSubcommandType(perf_args[1]);
    if (perf_args[0] != "perf" || subcommand == PERF_COMMAND_UNSUPPORTED) {
      DEBUGD_ADD_ERROR(error, kUnsupportedPerfToolErrorName, kArgsError);
      return false;
    }
  } else if (!ValidateQuipperArguments(perf_args, subcommand, error)) {
    return false;
  }

  // This whole method is synchronous, so we create a subprocess, let it run to
  // completion, then gather up its output to return it.
  ProcessWithOutput process;
  process.SandboxAs("root", "root");
  if (!process.Init()) {
    DEBUGD_ADD_ERROR(error, kProcessErrorName,
                     "Process initialization failure.");
    return false;
  }

  AddQuipperArguments(&process, duration_secs, perf_args);

  std::string output_string;
  *status = process.Run();
  if (*status != 0) {
    output_string =
        base::StringPrintf("<process exited with status: %d>", *status);
  } else {
    process.GetOutput(&output_string);
  }

  switch (subcommand) {
    case PERF_COMMAND_RECORD:
    case PERF_COMMAND_MEM:
      perf_data->assign(output_string.begin(), output_string.end());
      break;
    case PERF_COMMAND_STAT:
      perf_stat->assign(output_string.begin(), output_string.end());
      break;
    default:
      // Discard the output.
      break;
  }

  return true;
}

void PerfTool::OnQuipperProcessExited(const siginfo_t& siginfo) {
  // Called after SIGCHLD has been received from the signalfd file descriptor.
  // Wait() for the child process wont' block. It'll just reap the zombie child
  // process.
  quipper_process_->Wait();
  quipper_process_ = nullptr;
  quipper_process_output_fd_.reset();

  profiler_session_id_.reset();

  if (!cpuidle_states_.empty())
    RestoreCpuIdleStates();
}

bool PerfTool::GetPerfOutputFd(uint32_t duration_secs,
                               const std::vector<std::string>& perf_args,
                               const base::ScopedFD& stdout_fd,
                               uint64_t* session_id,
                               brillo::ErrorPtr* error) {
  PerfSubcommand subcommand;
  if (duration_secs > 0) {  // legacy option style
    if (perf_args.size() < 2) {
      DEBUGD_ADD_ERROR(error, kUnsupportedPerfToolErrorName, kArgsError);
      return false;
    }
    subcommand = GetPerfSubcommandType(perf_args[1]);
    if (perf_args[0] != "perf" || subcommand == PERF_COMMAND_UNSUPPORTED) {
      DEBUGD_ADD_ERROR(error, kUnsupportedPerfToolErrorName, kArgsError);
      return false;
    }
  } else if (!ValidateQuipperArguments(perf_args, subcommand, error)) {
    return false;
  }

  if (perf_running()) {
    // Do not run multiple sessions at the same time. Attempt to start another
    // profiler session using this method yields a DBus error. Note that
    // starting another session using GetPerfOutput() will still succeed.
    DEBUGD_ADD_ERROR(error, kProcessErrorName, "Existing perf tool running.");
    return false;
  }

  DCHECK(!profiler_session_id_);

  auto quipper_process = std::make_unique<SandboxedProcess>();
  quipper_process->SandboxAs("root", "root");
  if (!quipper_process->Init()) {
    DEBUGD_ADD_ERROR(error, kProcessErrorName,
                     "Process initialization failure.");
    return false;
  }

  AddQuipperArguments(quipper_process.get(), duration_secs, perf_args);
  quipper_process->BindFd(stdout_fd.get(), 1);

  if (!quipper_process->Start()) {
    DEBUGD_ADD_ERROR(error, kProcessErrorName, "Process start failure.");
    return false;
  }
  quipper_process_ = std::move(quipper_process);
  DCHECK_GT(quipper_process_->pid(), 0);

  process_reaper_.WatchForChild(
      FROM_HERE, quipper_process_->pid(),
      base::BindOnce(&PerfTool::OnQuipperProcessExited,
                     base::Unretained(this)));

  // When GetPerfOutputFd() is used to run the perf tool, the user will read
  // from the read end of |stdout_fd| until the write end is closed.  At that
  // point, it may make another call to GetPerfOutputFd() and expect that will
  // start another perf run. |stdout_fd| will be closed when the last process
  // holding it exits, which is minijail0 in this case. However, the kernel
  // closes fds before signaling process exit. Therefore, it's possible for
  // |stdout_fd| to be closed and the user tries to run another
  // GetPerfOutputFd() before we're signaled of the process exit. To mitigate
  // this, hold on to a dup() of |stdout_fd| until we're signaled that the
  // process has exited. This guarantees that the caller can make a new
  // GetPerfOutputFd() call when it finishes reading the output.
  quipper_process_output_fd_.reset(dup(stdout_fd.get()));
  DCHECK(quipper_process_output_fd_.is_valid());

  // Generate an opaque, pseudo-unique, session ID using time and process ID.
  profiler_session_id_ = *session_id =
      static_cast<uint64_t>(base::Time::Now().ToTimeT()) << 32 |
      (quipper_process_->pid() & 0xffffffff);

  return true;
}

bool PerfTool::StopPerf(uint64_t session_id, brillo::ErrorPtr* error) {
  if (!profiler_session_id_) {
    DEBUGD_ADD_ERROR(error, kStopProcessErrorName, "Perf tool not started");
    return false;
  }

  if (profiler_session_id_ != session_id) {
    // Session ID mismatch: return a failure without affecting the existing
    // profiler session.
    DEBUGD_ADD_ERROR(error, kStopProcessErrorName,
                     "Invalid profile session id.");
    return false;
  }

  // Stop by sending SIGINT to the profiler session. The sandboxed quipper
  // process will be reaped in OnQuipperProcessExited().
  if (quipper_process_) {
    DCHECK_GT(quipper_process_->pid(), 0);
    if (kill(quipper_process_->pid(), SIGINT) != 0) {
      PLOG(WARNING) << "Failed to stop the profiler session.";
    }
  }

  return true;
}

void PerfTool::EtmStrobbingSettings() {
  const base::FilePath window_path = base::FilePath(
      base::StringPrintf(kStrobbingSettingPathPattern, "window"));
  const base::FilePath period_path = base::FilePath(
      base::StringPrintf(kStrobbingSettingPathPattern, "period"));
  if (!base::PathExists(window_path) || !base::PathExists(period_path))
    return;

  base::WriteFile(window_path, std::to_string(kStrobbingWindow));
  base::WriteFile(period_path, std::to_string(kStrobbingPeriod));
  etm_available = true;
}

}  // namespace debugd
