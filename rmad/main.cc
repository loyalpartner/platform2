// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <sys/capability.h>
#include <sys/mount.h>

#include <brillo/syslog_logging.h>
#include <libminijail.h>
#include <scoped_minijail.h>

#include "rmad/dbus_service.h"
#include "rmad/rmad_interface_impl.h"
#include "rmad/utils/crossystem_utils_impl.h"

namespace {

constexpr char kRmadUser[] = "rmad";
constexpr char kRmadGroup[] = "rmad";
constexpr char kSeccompFilterPath[] = "/usr/share/policy/rmad-seccomp.policy";

}  // namespace

void EnterMinijail() {
  ScopedMinijail j(minijail_new());
  minijail_no_new_privs(j.get());
  minijail_remount_proc_readonly(j.get());
  minijail_namespace_ipc(j.get());
  minijail_namespace_net(j.get());
  minijail_namespace_uts(j.get());
  minijail_namespace_vfs(j.get());

  minijail_change_user(j.get(), kRmadUser);
  minijail_change_group(j.get(), kRmadGroup);
  minijail_inherit_usergroups(j.get());

  minijail_enter_pivot_root(j.get(), "/mnt/empty");

  minijail_mount_tmp(j.get());
  minijail_bind(j.get(), "/", "/", 0);
  minijail_bind(j.get(), "/dev/", "/dev", 0);
  minijail_bind(j.get(), "/proc", "/proc", 0);

  minijail_mount_with_data(j.get(), "tmpfs", "/run", "tmpfs", 0, nullptr);
  minijail_bind(j.get(), "/run/dbus", "/run/dbus", 0);

  minijail_mount_with_data(j.get(), "tmpfs", "/var", "tmpfs", 0, nullptr);
  minijail_bind(j.get(), "/var/lib/rmad", "/var/lib/rmad", 1);

  minijail_mount_with_data(j.get(), "tmpfs", "/sys", "tmpfs", 0, nullptr);
  minijail_bind(j.get(), "/sys/devices", "/sys/devices", 0);
  minijail_bind(j.get(), "/sys/class", "/sys/class", 0);

  rmad::CrosSystemUtilsImpl crossystem_utils;
  int wpsw_cur;
  if (crossystem_utils.GetInt("wpsw_cur", &wpsw_cur) && wpsw_cur == 0) {
    LOG(INFO) << "Hardware write protection off.";
    minijail_use_caps(
        j.get(), CAP_TO_MASK(CAP_SYS_RAWIO) | CAP_TO_MASK(CAP_DAC_OVERRIDE));
    minijail_set_ambient_caps(j.get());
    minijail_bind(j.get(), "/dev/mem", "/dev/mem", 0);
  } else {
    LOG(INFO) << "Hardware write protection on.";
  }

  minijail_use_seccomp_filter(j.get());
  minijail_parse_seccomp_filters(j.get(), kSeccompFilterPath);

  minijail_enter(j.get());
}

int main(int argc, char* argv[]) {
  brillo::InitLog(brillo::kLogToSyslog | brillo::kLogToStderrIfTty);

  rmad::RmadInterfaceImpl rmad_interface;
  rmad::DBusService dbus_service(&rmad_interface);

  LOG(INFO) << "Starting Chrome OS RMA Daemon.";
  EnterMinijail();
  return dbus_service.Run();
}