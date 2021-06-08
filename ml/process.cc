// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ml/process.h"

#include <utility>

#include <signal.h>
#include <sysexits.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pwd.h>

#include <base/bind.h>
#include <base/logging.h>
#include <base/process/process_metrics.h>
#include <base/strings/string_number_conversions.h>
#include <base/strings/string_util.h>
#include <base/task/post_task.h>
#include <base/time/time.h>
#include <libminijail.h>
#include <mojo/core/embedder/embedder.h>
#include <mojo/public/cpp/platform/platform_channel.h>
#include <mojo/public/cpp/system/invitation.h>
#include <scoped_minijail.h>

#include "ml/daemon.h"
#include "ml/machine_learning_service_impl.h"
#include "ml/request_metrics.h"
#include "ml/time_metrics.h"

namespace ml {

namespace {
constexpr char kMojoBootstrapFdSwitchName[] = "mojo-bootstrap-fd";

constexpr char kInternalMojoPrimordialPipeName[] = "cros_ml";

constexpr char kDefaultMlServiceBinaryPath[] = "/usr/bin/ml_service";

constexpr uid_t kMlServiceDBusUid = 20177;

// This is the maximum of re-trials we will reap a child process.
constexpr unsigned int kMaxNumOfWaitPidRetrials = 5;

// The delay time in trying to reap worker process.
constexpr int kWaitPidRetrialDelayTimesMilliseconds[kMaxNumOfWaitPidRetrials] =
    {100, 300, 1000, 3000, 10000};

std::string GetSeccompPolicyPath(const std::string& model_name) {
  return "/usr/share/policy/ml_service-" + model_name + "-seccomp.policy";
}

std::string GetArgumentForWorkerProcess(int fd) {
  std::string fd_argv = kMojoBootstrapFdSwitchName;
  return "--" + fd_argv + "=" + std::to_string(fd);
}

}  // namespace

// static
Process* Process::GetInstance() {
  // This is thread-safe.
  static base::NoDestructor<Process> instance;
  return instance.get();
}

int Process::Run(int argc, char* argv[]) {
  // Parses the command line and determines the process type.
  base::CommandLine command_line(argc, argv);
  std::string mojo_fd_string =
      command_line.GetSwitchValueASCII(kMojoBootstrapFdSwitchName);

  if (mojo_fd_string.empty()) {
    process_type_ = Type::kControl;
  } else {
    process_type_ = Type::kWorker;
  }

  if (!command_line.GetArgs().empty()) {
    LOG(ERROR) << "Unexpected command line arguments: "
               << base::JoinString(command_line.GetArgs(), "\t");
    return ExitCode::kUnexpectedCommandLine;
  }

  if (process_type_ == Type::kControl) {
    ControlProcessRun();
  } else {
    // The process type is either "control" or "worker".
    DCHECK(GetType() == Type::kWorker);
    const auto is_valid_fd_str =
        base::StringToInt(mojo_fd_string, &mojo_bootstrap_fd_);
    DCHECK(is_valid_fd_str) << "Invalid mojo bootstrap fd";
    WorkerProcessRun();
  }

  return ExitCode::kSuccess;
}

Process::Type Process::GetType() {
  return process_type_;
}

bool Process::SpawnWorkerProcessAndGetPid(const mojo::PlatformChannel& channel,
                                          const std::string& model_name,
                                          pid_t* worker_pid) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(worker_pid != nullptr);
  // Should only be called by the control process.
  DCHECK(IsControlProcess()) << "Should only be called by the control process";

  // Start the process.
  ScopedMinijail jail(minijail_new());

  minijail_namespace_ipc(jail.get());
  minijail_namespace_uts(jail.get());
  minijail_namespace_net(jail.get());
  minijail_namespace_cgroups(jail.get());

  // The following sandboxing makes unit test crash so we do not use them in
  // unit tests.
  if (process_type_ != Type::kControlForTest) {
    minijail_namespace_pids(jail.get());
    minijail_namespace_vfs(jail.get());
    std::string seccomp_policy_path = GetSeccompPolicyPath(model_name);
    minijail_parse_seccomp_filters(jail.get(), seccomp_policy_path.c_str());
    minijail_use_seccomp_filter(jail.get());
  }

  // This is the file descriptor used to bootstrap mojo connection between
  // control and worker processes.
  // Use GetFD instead of TakeFD to non-destructively obtain the fd.
  auto mojo_bootstrap_fd =
      channel.remote_endpoint().platform_handle().GetFD().get();

  // Closes the unused FDs in the worker process.
  // We keep the standard FDs here (should all point to `/dev/null`).
  // Also we need to keep the FD used in bootstrapping the mojo connection.
  minijail_preserve_fd(jail.get(), STDIN_FILENO, STDIN_FILENO);
  minijail_preserve_fd(jail.get(), STDOUT_FILENO, STDOUT_FILENO);
  minijail_preserve_fd(jail.get(), STDERR_FILENO, STDERR_FILENO);
  minijail_preserve_fd(jail.get(), mojo_bootstrap_fd, mojo_bootstrap_fd);
  minijail_close_open_fds(jail.get());

  std::string fd_argv = kMojoBootstrapFdSwitchName;
  fd_argv = GetArgumentForWorkerProcess(mojo_bootstrap_fd);
  char* const argv[3] = {&ml_service_path_[0], &fd_argv[0], nullptr};

  if (minijail_run_pid(jail.get(), &ml_service_path_[0], argv, worker_pid) !=
      0) {
    RecordProcessErrorEvent(ProcessError::kSpawnWorkerProcessFailed);
    LOG(DFATAL) << "Failed to spawn worker process for " << model_name;
    return false;
  }

  return true;
}

mojo::Remote<chromeos::machine_learning::mojom::MachineLearningService>&
Process::SendMojoInvitationAndGetRemote(pid_t worker_pid,
                                        mojo::PlatformChannel channel,
                                        const std::string& model_name) {
  // Send the Mojo invitation to the worker process.
  mojo::OutgoingInvitation invitation;
  mojo::ScopedMessagePipeHandle pipe =
      invitation.AttachMessagePipe(kInternalMojoPrimordialPipeName);

  mojo::Remote<chromeos::machine_learning::mojom::MachineLearningService>
      remote(mojo::PendingRemote<
             chromeos::machine_learning::mojom::MachineLearningService>(
          std::move(pipe), 0u /* version */));

  mojo::OutgoingInvitation::Send(std::move(invitation), worker_pid,
                                 channel.TakeLocalEndpoint());

  remote.set_disconnect_handler(
      base::BindOnce(&Process::InternalPrimordialMojoPipeDisconnectHandler,
                     base::Unretained(this), worker_pid));

  DCHECK(worker_pid_info_map_.find(worker_pid) == worker_pid_info_map_.end())
      << "Worker pid already exists";

  WorkerInfo worker_info;
  worker_info.remote = std::move(remote);
  worker_info.process_metrics =
      base::ProcessMetrics::CreateProcessMetrics(worker_pid);
  // Baseline the CPU usage counter in `process_metrics` to be zero as of now.
  worker_info.process_metrics->GetPlatformIndependentCPUUsage();

  worker_pid_info_map_[worker_pid] = std::move(worker_info);

  return worker_pid_info_map_[worker_pid].remote;
}

void Process::UnregisterWorkerProcess(pid_t pid) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const auto iter = worker_pid_info_map_.find(pid);
  DCHECK(iter != worker_pid_info_map_.end()) << "Pid is not registered";
  worker_pid_info_map_.erase(iter);
}

Process::Process()
    : process_type_(Type::kUnset),
      mojo_bootstrap_fd_(-1),
      ml_service_path_(kDefaultMlServiceBinaryPath) {}
Process::~Process() = default;

void Process::ControlProcessRun() {
  // We need to set euid to kMlServiceDBusUid to bootstrap DBus. Otherwise, DBus
  // will block us because our euid inside of the userns is 0 but is 20106
  // outside of the userns.
  if (seteuid(kMlServiceDBusUid) != 0) {
    RecordProcessErrorEvent(ProcessError::kChangeEuidToMlServiceDBusFailed);
    LOG(ERROR) << "Unable to change effective uid to " << kMlServiceDBusUid;
    exit(EX_OSERR);
  }

  ml::Daemon daemon;
  daemon.Run();
}

void Process::WorkerProcessRun() {
  brillo::BaseMessageLoop message_loop;
  message_loop.SetAsCurrent();
  DETACH_FROM_SEQUENCE(sequence_checker_);
  mojo::core::Init();
  mojo::core::ScopedIPCSupport ipc_support(
      base::ThreadTaskRunnerHandle::Get(),
      mojo::core::ScopedIPCSupport::ShutdownPolicy::FAST);
  mojo::IncomingInvitation invitation;
  {
    WallTimeMetric walltime_metric(
        "MachineLearningService.WorkerProcessAcceptMojoConnectionTime");
    invitation = mojo::IncomingInvitation::Accept(mojo::PlatformChannelEndpoint(
        mojo::PlatformHandle(base::ScopedFD(mojo_bootstrap_fd_))));
  }
  mojo::ScopedMessagePipeHandle pipe =
      invitation.ExtractMessagePipe(kInternalMojoPrimordialPipeName);
  // The worker process exits if it disconnects with the control process.
  // This can be important because in the control process's disconnect handler
  // function we will use waitpid to wait for this process to finish. So
  // the exit here will make sure that the waitpid in control process
  // won't hang.
  MachineLearningServiceImpl machine_learning_service_impl(
      mojo::PendingReceiver<
          chromeos::machine_learning::mojom::MachineLearningService>(
          std::move(pipe)),
      message_loop.QuitClosure());
  message_loop.Run();
}

const std::unordered_map<pid_t, Process::WorkerInfo>&
Process::GetWorkerPidInfoMap() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return worker_pid_info_map_;
}

void Process::SetTypeForTesting(Type type) {
  process_type_ = type;
}

void Process::SetMlServicePathForTesting(const std::string& path) {
  ml_service_path_ = path;
}

void Process::SetReapWorkerProcessSucceedCallbackForTesting(
    base::RepeatingClosure callback) {
  reap_worker_process_succeed_callback_ = std::move(callback);
}

void Process::SetReapWorkerProcessFailCallbackForTesting(
    base::RepeatingCallback<void(std::string reason)> callback) {
  reap_worker_process_fail_callback_ = std::move(callback);
}

bool Process::IsControlProcess() {
  return process_type_ == Type::kControl ||
         process_type_ == Type::kControlForTest;
}

bool Process::IsWorkerProcess() {
  return process_type_ == Type::kWorker ||
         process_type_ == Type::kSingleProcessForTest;
}

void Process::ReapWorkerProcess(pid_t child_pid,
                                int times_tried,
                                base::Time begin_time) {
  if (times_tried >= kMaxNumOfWaitPidRetrials) {
    // Tried too many time, give up on reaping child process and report an
    // error.
    RecordProcessErrorEvent(
        ProcessError::kReapWorkerProcessMaxNumOfRetrialsExceeded);
    LOG(ERROR) << "Max number of retrials (" << kMaxNumOfWaitPidRetrials
               << ") exceeded in trying to reap the worker process";
    if (process_type_ == Type::kControlForTest &&
        !reap_worker_process_fail_callback_.is_null()) {
      reap_worker_process_fail_callback_.Run("Max number of retrials exceeded");
    }
    return;
  }

  // Reap the worker process.
  int status;
  const pid_t ret_pid = waitpid(child_pid, &status, WNOHANG);
  if (ret_pid > 0) {
    // Worker process has exited and been correctly reaped.
    DCHECK(ret_pid == child_pid);
    UnregisterWorkerProcess(child_pid);
    int exit_status = WEXITSTATUS(status);
    if (exit_status != 0) {
      RecordWorkerProcessExitStatus(WEXITSTATUS(status));
    }
    // Record how long it takes to reap the worker process.
    RecordReapWorkerProcessWallTime(begin_time, base::Time::Now());
    // Call the "succeed callback" used in testing.
    if (process_type_ == Type::kControlForTest &&
        !reap_worker_process_succeed_callback_.is_null()) {
      reap_worker_process_succeed_callback_.Run();
    }
    return;
  } else if (ret_pid == 0) {
    // The worker process hasn't exited yet.
    DCHECK(times_tried < sizeof(kWaitPidRetrialDelayTimesMilliseconds) /
                             sizeof(kWaitPidRetrialDelayTimesMilliseconds[0]));
    // Try to reap the process again after some time.
    base::ThreadTaskRunnerHandle::Get()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&Process::ReapWorkerProcess, base::Unretained(this),
                       child_pid, times_tried + 1, begin_time),
        base::TimeDelta::FromMilliseconds(
            kWaitPidRetrialDelayTimesMilliseconds[times_tried]));
    return;
  } else {
    // Records the errno first to avoid it being changed.
    RecordReapWorkerProcessErrno(errno);
    LOG(ERROR) << "waitpid met error with errno: " << errno;

    // Call the "fail callback" used in testing.
    if (process_type_ == Type::kControlForTest &&
        !reap_worker_process_fail_callback_.is_null()) {
      reap_worker_process_fail_callback_.Run("waitpid met error with errno: " +
                                             std::to_string(errno));
    }
  }
}

void Process::InternalPrimordialMojoPipeDisconnectHandler(pid_t child_pid) {
  // Try our best to ensure the worker process is exiting.
  kill(child_pid, SIGKILL);
  // Reap the child process. This is (and should be) unblocking.
  ReapWorkerProcess(child_pid, 0, base::Time::Now());
}

}  // namespace ml