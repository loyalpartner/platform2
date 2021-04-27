/*
 * Copyright 2016 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "hal_adapter/camera_module_callbacks_delegate.h"

#include <utility>

#include <base/bind.h>
#include <base/callback_helpers.h>
#include <base/check.h>

#include "cros-camera/common.h"
#include "cros-camera/future.h"

namespace cros {

CameraModuleCallbacksDelegate::CameraModuleCallbacksDelegate(
    scoped_refptr<base::SingleThreadTaskRunner> task_runner)
    : internal::MojoRemote<mojom::CameraModuleCallbacks>(task_runner) {}

void CameraModuleCallbacksDelegate::CameraDeviceStatusChange(int camera_id,
                                                             int new_status) {
  VLOGF_ENTER();
  auto future = cros::Future<void>::Create(&relay_);
  task_runner_->PostTask(
      FROM_HERE,
      base::Bind(
          &CameraModuleCallbacksDelegate::CameraDeviceStatusChangeOnThread,
          base::AsWeakPtr(this), camera_id, new_status,
          cros::GetFutureCallback(future)));
  future->Wait();
}

void CameraModuleCallbacksDelegate::TorchModeStatusChange(int camera_id,
                                                          int new_status) {
  VLOGF_ENTER();
  auto future = cros::Future<void>::Create(&relay_);
  task_runner_->PostTask(
      FROM_HERE,
      base::Bind(&CameraModuleCallbacksDelegate::TorchModeStatusChangeOnThread,
                 base::AsWeakPtr(this), camera_id, new_status,
                 cros::GetFutureCallback(future)));
  future->Wait();
}

void CameraModuleCallbacksDelegate::CameraDeviceStatusChangeOnThread(
    int camera_id, int new_status, base::Closure callback) {
  VLOGF_ENTER();
  DCHECK(task_runner_->BelongsToCurrentThread());
  remote_->CameraDeviceStatusChange(
      camera_id, static_cast<mojom::CameraDeviceStatus>(new_status));
  callback.Run();
}

void CameraModuleCallbacksDelegate::TorchModeStatusChangeOnThread(
    int camera_id, int new_status, base::Closure callback) {
  VLOGF_ENTER();
  DCHECK(task_runner_->BelongsToCurrentThread());
  remote_->TorchModeStatusChange(
      camera_id, static_cast<mojom::TorchModeStatus>(new_status));
  callback.Run();
}

}  // namespace cros