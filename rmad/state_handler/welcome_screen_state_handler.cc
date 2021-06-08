// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "rmad/state_handler/welcome_screen_state_handler.h"

#include "base/notreached.h"

namespace rmad {

WelcomeScreenStateHandler::WelcomeScreenStateHandler(
    scoped_refptr<JsonStore> json_store)
    : BaseStateHandler(json_store) {}

RmadErrorCode WelcomeScreenStateHandler::InitializeState() {
  if (!state_.has_welcome() && !RetrieveState()) {
    state_.set_allocated_welcome(new WelcomeState);
  }
  return RMAD_ERROR_OK;
}

BaseStateHandler::GetNextStateCaseReply
WelcomeScreenStateHandler::GetNextStateCase(const RmadState& state) {
  if (!state.has_welcome()) {
    LOG(ERROR) << "RmadState missing |welcome| state.";
    return {.error = RMAD_ERROR_REQUEST_INVALID, .state_case = GetStateCase()};
  }
  const WelcomeState& welcome = state.welcome();
  if (welcome.choice() == WelcomeState::RMAD_CHOICE_UNKNOWN) {
    return {.error = RMAD_ERROR_REQUEST_ARGS_MISSING,
            .state_case = GetStateCase()};
  }

  state_ = state;
  StoreState();

  switch (state_.welcome().choice()) {
    case WelcomeState::RMAD_CHOICE_CANCEL:
      return {.error = RMAD_ERROR_RMA_NOT_REQUIRED,
              .state_case = RmadState::StateCase::STATE_NOT_SET};
    case WelcomeState::RMAD_CHOICE_FINALIZE_REPAIR:
      return {.error = RMAD_ERROR_OK,
              .state_case = RmadState::StateCase::kSelectNetwork};
    default:
      break;
  }
  NOTREACHED();
  return {.error = RMAD_ERROR_NOT_SET,
          .state_case = RmadState::StateCase::STATE_NOT_SET};
}

}  // namespace rmad