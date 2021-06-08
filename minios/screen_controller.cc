// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "minios/screen_controller.h"

#include <utility>

#include <base/logging.h>

#include "minios/screen_language_dropdown.h"
#include "minios/screen_network.h"
#include "minios/screen_welcome.h"
#include "minios/screens/screen_error.h"

namespace minios {

ScreenController::ScreenController(
    std::shared_ptr<DrawInterface> draw_utils,
    std::shared_ptr<NetworkManagerInterface> network_manager)
    : key_reader_(KeyReader{/*include_usb=*/true}),
      draw_utils_(draw_utils),
      network_manager_(network_manager),
      key_states_(kFdsMax, std::vector<bool>(kKeyMax, false)) {}

void ScreenController::Init() {
  CHECK(draw_utils_)
      << "Screen drawing utility not available. Cannot continue.";

  draw_utils_->Init();

  std::vector<int> wait_keys = {kKeyUp, kKeyDown, kKeyEnter};
  if (draw_utils_->IsDetachable())
    wait_keys = {kKeyVolDown, kKeyVolUp, kKeyPower};
  CHECK(key_reader_.Init(wait_keys))
      << "Could not initialize key reader. Unable to continue.";

  key_reader_.SetDelegate(this);

  current_screen_ = CreateScreen(ScreenType::kWelcomeScreen);
  current_screen_->Show();
}

std::unique_ptr<ScreenInterface> ScreenController::CreateScreen(
    ScreenType screen_type) {
  switch (screen_type) {
    case ScreenType::kWelcomeScreen:
      return std::make_unique<ScreenWelcome>(draw_utils_, this);
    case ScreenType::kNetworkDropDownScreen:
      return std::make_unique<ScreenNetwork>(draw_utils_, network_manager_,
                                             this);
    case ScreenType::kLanguageDropDownScreen:
      return std::make_unique<ScreenLanguageDropdown>(draw_utils_, this);
    case ScreenType::kDownloadError:
    case ScreenType::kNetworkError:
    case ScreenType::kPasswordError:
    case ScreenType::kConnectionError:
    case ScreenType::kGeneralError:
      return std::make_unique<ScreenError>(screen_type, draw_utils_, this);
    default:
      // TODO(vyshu) : Other screens not yet implemented. Once all screens are
      // done, this should never return nullptr.
      return nullptr;
  }
}

void ScreenController::OnForward(ScreenInterface* screen) {
  switch (screen->GetType()) {
    case ScreenType::kWelcomeScreen:
      current_screen_ = CreateScreen(ScreenType::kNetworkDropDownScreen);
      break;
    case ScreenType::kDownloadError:
    case ScreenType::kNetworkError:
    case ScreenType::kPasswordError:
    case ScreenType::kConnectionError:
    case ScreenType::kGeneralError:
    // Show debug options and log screen.
    // TODO(vyshu) : kDebugOptionsScreen not yet implemented.
    default:
      // TODO(vyshu) : Other screens not yet implemented.
      break;
  }
  current_screen_->Show();
}

void ScreenController::OnBackward(ScreenInterface* screen) {
  switch (screen->GetType()) {
    case ScreenType::kWelcomeScreen:
    case ScreenType::kExpandedNetworkDropDownScreen:
      // Not moving to a new screen. Just reset the state of the current screen.
      current_screen_->Reset();
      break;
    case ScreenType::kNetworkDropDownScreen:
      current_screen_ = CreateScreen(ScreenType::kWelcomeScreen);
      break;
    case ScreenType::kPasswordError:
      // Return to network screen if it was the previous screen otherwise create
      // a new one.
      if (previous_screen_ &&
          previous_screen_->GetType() == ScreenType::kNetworkDropDownScreen) {
        current_screen_ = std::move(previous_screen_);
      } else {
        current_screen_ = CreateScreen(ScreenType::kNetworkDropDownScreen);
      }
      break;
    case ScreenType::kNetworkError:
    case ScreenType::kConnectionError:
      // Return to network dropdown screen.
      current_screen_ = CreateScreen(ScreenType::kNetworkDropDownScreen);
      break;
    case ScreenType::kDownloadError:
    case ScreenType::kGeneralError:
      // Return to beginning of the flow.
      current_screen_ = CreateScreen(ScreenType::kWelcomeScreen);
      break;
    default:
      // TODO(vyshu) : Other screens not yet implemented.
      break;
  }
  current_screen_->Show();
}

void ScreenController::OnError(ScreenType error_screen) {
  switch (error_screen) {
    case ScreenType::kDownloadError:
    case ScreenType::kNetworkError:
    case ScreenType::kPasswordError:
    case ScreenType::kConnectionError:
    case ScreenType::kGeneralError:
      previous_screen_ = std::move(current_screen_);
      current_screen_ = CreateScreen(error_screen);
      break;
    default:
      LOG(WARNING)
          << "Not a valid error screen. Defaulting to general error case.";
      previous_screen_ = std::move(current_screen_);
      current_screen_ = CreateScreen(ScreenType::kGeneralError);
      break;
  }
  current_screen_->Show();
}

ScreenType ScreenController::GetCurrentScreen() {
  return current_screen_->GetType();
}

void ScreenController::SwitchLocale(ScreenInterface* screen) {
  previous_screen_ = std::move(current_screen_);
  current_screen_ = CreateScreen(ScreenType::kLanguageDropDownScreen);
  current_screen_->Show();
}

void ScreenController::UpdateLocale(ScreenInterface* screen,
                                    int selected_locale_index) {
  // Change locale and update constants.
  CHECK(draw_utils_) << "Screen drawing utility not available.";
  if (screen->GetType() != ScreenType::kLanguageDropDownScreen) {
    LOG(WARNING) << "Only the language dropdown screen can change the locale.";
    return;
  }
  draw_utils_->LocaleChange(selected_locale_index);
  current_screen_ = std::move(previous_screen_);
  current_screen_->Reset();
  current_screen_->Show();
}

void ScreenController::OnKeyPress(int fd_index,
                                  int key_changed,
                                  bool key_released) {
  CHECK(current_screen_) << "Could not send key event to screen.";

  // Make sure you have seen a key press for this key before ending on key
  // event release.
  if (fd_index < 0 || key_changed < 0 || fd_index >= key_states_.size() ||
      key_changed >= key_states_[0].size()) {
    LOG(ERROR) << "Fd index or key code out of range.  Index: " << fd_index
               << ". Key code: " << key_changed;
    return;
  }

  if (key_released && key_states_[fd_index][key_changed]) {
    key_states_[fd_index][key_changed] = false;
    // Send key event to the currently displayed screen. It will decide what to
    // do with it.
    current_screen_->OnKeyPress(key_changed);
    return;
  } else if (!key_released) {
    key_states_[fd_index][key_changed] = true;
  }
}

}  // namespace minios