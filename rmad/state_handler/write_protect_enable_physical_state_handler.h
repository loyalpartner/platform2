// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef RMAD_STATE_HANDLER_WRITE_PROTECT_ENABLE_PHYSICAL_STATE_HANDLER_H_
#define RMAD_STATE_HANDLER_WRITE_PROTECT_ENABLE_PHYSICAL_STATE_HANDLER_H_

#include "rmad/state_handler/base_state_handler.h"

namespace rmad {

class WriteProtectEnablePhysicalStateHandler : public BaseStateHandler {
 public:
  explicit WriteProtectEnablePhysicalStateHandler(
      scoped_refptr<JsonStore> json_store);
  ~WriteProtectEnablePhysicalStateHandler() override = default;

  ASSIGN_STATE(RmadState::StateCase::kWpEnablePhysical);
  SET_REPEATABLE;

  RmadErrorCode InitializeState() override;
  GetNextStateCaseReply GetNextStateCase(const RmadState& state) override;
};

}  // namespace rmad

#endif  // RMAD_STATE_HANDLER_WRITE_PROTECT_ENABLE_PHYSICAL_STATE_HANDLER_H_
