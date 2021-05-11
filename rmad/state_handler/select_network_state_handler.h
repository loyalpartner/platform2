// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef RMAD_STATE_HANDLER_SELECT_NETWORK_STATE_HANDLER_H_
#define RMAD_STATE_HANDLER_SELECT_NETWORK_STATE_HANDLER_H_

#include "rmad/state_handler/base_state_handler.h"

namespace rmad {

class SelectNetworkStateHandler : public BaseStateHandler {
 public:
  explicit SelectNetworkStateHandler(scoped_refptr<JsonStore> json_store);
  ~SelectNetworkStateHandler() override = default;

  ASSIGN_STATE(RmadState::StateCase::kSelectNetwork);
  SET_REPEATABLE;

  GetNextStateCaseReply GetNextStateCase(const RmadState& state) override;
  RmadErrorCode ResetState() override;
};

}  // namespace rmad

#endif  // RMAD_STATE_HANDLER_SELECT_NETWORK_STATE_HANDLER_H_
