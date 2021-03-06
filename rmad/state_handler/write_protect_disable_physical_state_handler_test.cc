// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <base/memory/scoped_refptr.h>
#include <base/test/task_environment.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "rmad/constants.h"
#include "rmad/state_handler/state_handler_test_common.h"
#include "rmad/state_handler/write_protect_disable_physical_state_handler.h"
#include "rmad/system/mock_power_manager_client.h"
#include "rmad/utils/mock_cr50_utils.h"
#include "rmad/utils/mock_crossystem_utils.h"

using testing::_;
using testing::Assign;
using testing::DoAll;
using testing::Eq;
using testing::InSequence;
using testing::IsFalse;
using testing::NiceMock;
using testing::Return;
using testing::SetArgPointee;
using testing::StrictMock;

namespace rmad {

class WriteProtectDisablePhysicalStateHandlerTest : public StateHandlerTest {
 public:
  // Helper class to mock the callback function to send signal.
  class SignalSender {
   public:
    MOCK_METHOD(void, SendHardwareWriteProtectSignal, (bool), (const));
  };

  scoped_refptr<WriteProtectDisablePhysicalStateHandler> CreateStateHandler(
      const std::vector<int> wp_status_list,
      bool factory_mode_enabled,
      bool enable_factory_mode_success,
      bool* factory_mode_toggled = nullptr,
      bool* reboot_toggled = nullptr) {
    // Mock |Cr50Utils|, |CrosSystemUtils|, |CryptohomeClient| and
    // |PowerManagerClient|.
    auto mock_crossystem_utils =
        std::make_unique<StrictMock<MockCrosSystemUtils>>();
    {
      InSequence seq;
      for (int i = 0; i < wp_status_list.size(); ++i) {
        EXPECT_CALL(*mock_crossystem_utils,
                    GetInt(Eq(CrosSystemUtils::kHwwpStatusProperty), _))
            .WillOnce(DoAll(SetArgPointee<1>(wp_status_list[i]), Return(true)));
      }
    }

    auto mock_cr50_utils = std::make_unique<NiceMock<MockCr50Utils>>();
    ON_CALL(*mock_cr50_utils, IsFactoryModeEnabled())
        .WillByDefault(Return(factory_mode_enabled));
    if (factory_mode_toggled) {
      ON_CALL(*mock_cr50_utils, EnableFactoryMode())
          .WillByDefault(DoAll(Assign(factory_mode_toggled, true),
                               Return(enable_factory_mode_success)));
    }

    auto mock_power_manager_client =
        std::make_unique<NiceMock<MockPowerManagerClient>>();
    if (reboot_toggled) {
      ON_CALL(*mock_power_manager_client, Restart())
          .WillByDefault(DoAll(Assign(reboot_toggled, true), Return(true)));
    }

    auto handler =
        base::MakeRefCounted<WriteProtectDisablePhysicalStateHandler>(
            json_store_, std::move(mock_cr50_utils),
            std::move(mock_crossystem_utils),
            std::move(mock_power_manager_client));
    auto callback =
        base::BindRepeating(&SignalSender::SendHardwareWriteProtectSignal,
                            base::Unretained(&signal_sender_));
    handler->RegisterSignalSender(callback);
    return handler;
  }

 protected:
  StrictMock<SignalSender> signal_sender_;
  bool factory_mode_enabled_;

  // Variables for TaskRunner.
  base::test::SingleThreadTaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
};

TEST_F(WriteProtectDisablePhysicalStateHandlerTest, InitializeState_Success) {
  EXPECT_TRUE(json_store_->SetValue(kWipeDevice, false));
  auto handler = CreateStateHandler({0}, true, true);
  EXPECT_EQ(handler->InitializeState(), RMAD_ERROR_OK);
  handler->RunState();
  EXPECT_TRUE(handler->GetState().wp_disable_physical().keep_device_open());

  bool signal_sent = false;
  EXPECT_CALL(signal_sender_, SendHardwareWriteProtectSignal(IsFalse()))
      .WillOnce(Assign(&signal_sent, true));

  task_environment_.FastForwardBy(
      WriteProtectDisablePhysicalStateHandler::kPollInterval);
  EXPECT_TRUE(signal_sent);
}

TEST_F(WriteProtectDisablePhysicalStateHandlerTest, InitializeState_Failed) {
  // No kWipeDevice set in |json_store_|.
  auto handler = CreateStateHandler({}, true, true);
  EXPECT_EQ(handler->InitializeState(),
            RMAD_ERROR_STATE_HANDLER_INITIALIZATION_FAILED);
}

TEST_F(WriteProtectDisablePhysicalStateHandlerTest,
       GetNextStateCase_Success_CleanUpBeforeSignal) {
  EXPECT_TRUE(json_store_->SetValue(kWipeDevice, false));
  auto handler = CreateStateHandler({0}, true, true);
  EXPECT_EQ(handler->InitializeState(), RMAD_ERROR_OK);
  handler->RunState();

  RmadState state;
  state.set_allocated_wp_disable_physical(new WriteProtectDisablePhysicalState);

  auto [error, state_case] = handler->GetNextStateCase(state);
  EXPECT_EQ(error, RMAD_ERROR_OK);
  EXPECT_EQ(state_case, RmadState::StateCase::kWpDisableComplete);

  handler->CleanUpState();
  task_environment_.FastForwardBy(
      WriteProtectDisablePhysicalStateHandler::kPollInterval);
  // |signal_sender_| is not called.
}

TEST_F(WriteProtectDisablePhysicalStateHandlerTest,
       GetNextStateCase_Success_FactoryModeEnabled) {
  EXPECT_TRUE(json_store_->SetValue(kWipeDevice, true));
  auto handler = CreateStateHandler({0, 0}, true, true);
  EXPECT_EQ(handler->InitializeState(), RMAD_ERROR_OK);
  handler->RunState();

  RmadState state;
  state.set_allocated_wp_disable_physical(new WriteProtectDisablePhysicalState);

  auto [error, state_case] = handler->GetNextStateCase(state);
  EXPECT_EQ(error, RMAD_ERROR_OK);
  EXPECT_EQ(state_case, RmadState::StateCase::kWpDisableComplete);

  int wp_disable_method;
  EXPECT_TRUE(
      json_store_->GetValue(kWriteProtectDisableMethod, &wp_disable_method));
  EXPECT_EQ(
      wp_disable_method,
      static_cast<int>(WriteProtectDisableMethod::PHYSICAL_ASSEMBLE_DEVICE));

  bool signal_sent = false;
  EXPECT_CALL(signal_sender_, SendHardwareWriteProtectSignal(IsFalse()))
      .WillOnce(Assign(&signal_sent, true));

  task_environment_.FastForwardBy(
      WriteProtectDisablePhysicalStateHandler::kPollInterval);
  EXPECT_TRUE(signal_sent);
}

TEST_F(WriteProtectDisablePhysicalStateHandlerTest,
       GetNextStateCase_Success_KeepDeviceOpen) {
  EXPECT_TRUE(json_store_->SetValue(kWipeDevice, false));
  auto handler = CreateStateHandler({0, 0}, false, true);
  EXPECT_EQ(handler->InitializeState(), RMAD_ERROR_OK);
  handler->RunState();

  RmadState state;
  state.set_allocated_wp_disable_physical(new WriteProtectDisablePhysicalState);

  auto [error, state_case] = handler->GetNextStateCase(state);
  EXPECT_EQ(error, RMAD_ERROR_OK);
  EXPECT_EQ(state_case, RmadState::StateCase::kWpDisableComplete);

  int wp_disable_method;
  EXPECT_TRUE(
      json_store_->GetValue(kWriteProtectDisableMethod, &wp_disable_method));
  EXPECT_EQ(
      wp_disable_method,
      static_cast<int>(WriteProtectDisableMethod::PHYSICAL_KEEP_DEVICE_OPEN));

  bool signal_sent = false;
  EXPECT_CALL(signal_sender_, SendHardwareWriteProtectSignal(IsFalse()))
      .WillOnce(Assign(&signal_sent, true));

  task_environment_.FastForwardBy(
      WriteProtectDisablePhysicalStateHandler::kPollInterval);
  EXPECT_TRUE(signal_sent);
}

TEST_F(WriteProtectDisablePhysicalStateHandlerTest,
       GetNextStateCase_FactoryModeDisabled_EnableSuccess) {
  EXPECT_TRUE(json_store_->SetValue(kWipeDevice, true));
  bool factory_mode_toggled = false, reboot_toggled = false;
  auto handler = CreateStateHandler({1, 1, 0}, false, true,
                                    &factory_mode_toggled, &reboot_toggled);
  EXPECT_EQ(handler->InitializeState(), RMAD_ERROR_OK);
  handler->RunState();
  EXPECT_FALSE(handler->GetState().wp_disable_physical().keep_device_open());

  RmadState state;
  state.set_allocated_wp_disable_physical(new WriteProtectDisablePhysicalState);

  auto [error, state_case] = handler->GetNextStateCase(state);
  EXPECT_EQ(error, RMAD_ERROR_WAIT);
  EXPECT_EQ(state_case, RmadState::StateCase::kWpDisablePhysical);

  EXPECT_FALSE(factory_mode_toggled);
  EXPECT_FALSE(reboot_toggled);
  // First call to |mock_crossystem_utils_| during polling, get 1.
  task_environment_.FastForwardBy(
      WriteProtectDisablePhysicalStateHandler::kPollInterval);
  EXPECT_FALSE(factory_mode_toggled);
  EXPECT_FALSE(reboot_toggled);
  // Second call to |mock_crossystem_utils_| during polling, get 1.
  task_environment_.FastForwardBy(
      WriteProtectDisablePhysicalStateHandler::kPollInterval);
  EXPECT_FALSE(factory_mode_toggled);
  EXPECT_FALSE(reboot_toggled);
  // Third call to |mock_crossystem_utils_| during polling, get 0.
  task_environment_.FastForwardBy(
      WriteProtectDisablePhysicalStateHandler::kPollInterval);
  EXPECT_FALSE(factory_mode_toggled);
  EXPECT_FALSE(reboot_toggled);
  // Try to enable factory mode after a delay.
  task_environment_.FastForwardBy(
      WriteProtectDisablePhysicalStateHandler::kRebootDelay);
  EXPECT_TRUE(factory_mode_toggled);
  EXPECT_FALSE(reboot_toggled);
}

TEST_F(WriteProtectDisablePhysicalStateHandlerTest,
       GetNextStateCase_FactoryModeDisabled_EnableFailed) {
  EXPECT_TRUE(json_store_->SetValue(kWipeDevice, true));
  bool factory_mode_toggled = false, reboot_toggled = false;
  auto handler = CreateStateHandler({1, 1, 0}, false, false,
                                    &factory_mode_toggled, &reboot_toggled);
  EXPECT_EQ(handler->InitializeState(), RMAD_ERROR_OK);
  handler->RunState();
  EXPECT_FALSE(handler->GetState().wp_disable_physical().keep_device_open());

  RmadState state;
  state.set_allocated_wp_disable_physical(new WriteProtectDisablePhysicalState);

  auto [error, state_case] = handler->GetNextStateCase(state);
  EXPECT_EQ(error, RMAD_ERROR_WAIT);
  EXPECT_EQ(state_case, RmadState::StateCase::kWpDisablePhysical);

  EXPECT_FALSE(factory_mode_toggled);
  EXPECT_FALSE(reboot_toggled);
  // First call to |mock_crossystem_utils_| during polling, get 1.
  task_environment_.FastForwardBy(
      WriteProtectDisablePhysicalStateHandler::kPollInterval);
  EXPECT_FALSE(factory_mode_toggled);
  EXPECT_FALSE(reboot_toggled);
  // Second call to |mock_crossystem_utils_| during polling, get 1.
  task_environment_.FastForwardBy(
      WriteProtectDisablePhysicalStateHandler::kPollInterval);
  EXPECT_FALSE(factory_mode_toggled);
  EXPECT_FALSE(reboot_toggled);
  // Third call to |mock_crossystem_utils_| during polling, get 0.
  task_environment_.FastForwardBy(
      WriteProtectDisablePhysicalStateHandler::kPollInterval);
  EXPECT_FALSE(factory_mode_toggled);
  EXPECT_FALSE(reboot_toggled);
  // Try to enable factory mode after a delay.
  task_environment_.FastForwardBy(
      WriteProtectDisablePhysicalStateHandler::kRebootDelay);
  EXPECT_TRUE(factory_mode_toggled);
  EXPECT_TRUE(reboot_toggled);
}

TEST_F(WriteProtectDisablePhysicalStateHandlerTest,
       GetNextStateCase_MissingState) {
  EXPECT_TRUE(json_store_->SetValue(kWipeDevice, false));
  auto handler = CreateStateHandler({}, false, true);
  EXPECT_EQ(handler->InitializeState(), RMAD_ERROR_OK);

  // No WriteProtectDisablePhysicalState.
  RmadState state;

  auto [error, state_case] = handler->GetNextStateCase(state);
  EXPECT_EQ(error, RMAD_ERROR_REQUEST_INVALID);
  EXPECT_EQ(state_case, RmadState::StateCase::kWpDisablePhysical);
}

}  // namespace rmad
