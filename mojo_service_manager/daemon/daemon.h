// Copyright 2022 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MOJO_SERVICE_MANAGER_DAEMON_DAEMON_H_
#define MOJO_SERVICE_MANAGER_DAEMON_DAEMON_H_

#include <memory>

#include <base/files/file_descriptor_watcher_posix.h>
#include <base/files/file_path.h>
#include <base/files/scoped_file.h>
#include <brillo/daemons/daemon.h>
#include <mojo/core/embedder/scoped_ipc_support.h>

#include "mojo_service_manager/daemon/configuration.h"
#include "mojo_service_manager/daemon/service_manager.h"
#include "mojo_service_manager/daemon/service_policy.h"

namespace chromeos {
namespace mojo_service_manager {

// Sets up threading environment and initializes unix socket server of the
// service manager daemon.
class Daemon : public brillo::Daemon {
 public:
  Daemon(const base::FilePath& socket_path,
         Configuration configuration,
         ServicePolicyMap policy_map);
  Daemon(const Daemon&) = delete;
  Daemon& operator=(const Daemon&) = delete;
  ~Daemon() override;

 private:
  // ::brillo::Daemon overrides.
  int OnInit() override;
  void OnShutdown(int* exit_code) override;

  // Sends mojo invitation to the peer socket and binds the receiver of
  // mojom::ServiceManager.
  void SendMojoInvitationAndBindReceiver();

  // The |ScopedIPCSupport| instance for mojo.
  mojo::core::ScopedIPCSupport ipc_support_;
  // The path to the unix socket of the daemon.
  const base::FilePath socket_path_;
  // The fd of the unix socket server of the daemon.
  base::ScopedFD socket_fd_;
  // The fd watcher to monitor the socket server.
  std::unique_ptr<base::FileDescriptorWatcher::Controller> socket_watcher_;
  // Implements mojom::ServiceManager.
  ServiceManager service_manager_;
};

}  // namespace mojo_service_manager
}  // namespace chromeos

#endif  // MOJO_SERVICE_MANAGER_DAEMON_DAEMON_H_
