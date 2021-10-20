// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <sysexits.h>

#include <string>

#include <base/files/scoped_temp_dir.h>
#include <brillo/dbus/dbus_object_test_helpers.h>
#include <brillo/file_utils.h>
#include <dbus/mock_bus.h>
#include <dbus/mock_exported_object.h>
#include <dbus/rmad/dbus-constants.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "rmad/dbus_service.h"
#include "rmad/mock_rmad_interface.h"
#include "rmad/system/mock_tpm_manager_client.h"

using brillo::dbus_utils::AsyncEventSequencer;
using brillo::dbus_utils::PopValueFromReader;
using testing::_;
using testing::A;
using testing::DoAll;
using testing::Invoke;
using testing::NiceMock;
using testing::Return;
using testing::SetArgPointee;
using testing::StrictMock;

namespace rmad {

class DBusServiceTest : public testing::Test {
 public:
  DBusServiceTest() {
    dbus::Bus::Options options;
    mock_bus_ = base::MakeRefCounted<NiceMock<dbus::MockBus>>(options);
    dbus::ObjectPath path(kRmadServicePath);
    mock_exported_object_ =
        base::MakeRefCounted<NiceMock<dbus::MockExportedObject>>(
            mock_bus_.get(), path);
    ON_CALL(*mock_bus_, GetExportedObject(path))
        .WillByDefault(Return(mock_exported_object_.get()));

    EXPECT_CALL(mock_rmad_service_, GetCurrentStateCase())
        .WillRepeatedly(Return(RmadState::STATE_NOT_SET));
    EXPECT_CALL(
        mock_rmad_service_,
        RegisterSignalSender(
            _, A<std::unique_ptr<base::RepeatingCallback<bool(bool)>>>()))
        .WillRepeatedly(Return());
    EXPECT_CALL(
        mock_rmad_service_,
        RegisterSignalSender(_, A<std::unique_ptr<base::RepeatingCallback<bool(
                                    const HardwareVerificationResult&)>>>()))
        .WillRepeatedly(Return());
    EXPECT_CALL(
        mock_rmad_service_,
        RegisterSignalSender(
            _, A<std::unique_ptr<
                   base::RepeatingCallback<bool(CalibrationOverallStatus)>>>()))
        .WillRepeatedly(Return());
    EXPECT_CALL(
        mock_rmad_service_,
        RegisterSignalSender(
            _,
            A<std::unique_ptr<
                base::RepeatingCallback<bool(CalibrationComponentStatus)>>>()))
        .WillRepeatedly(Return());
    EXPECT_CALL(
        mock_rmad_service_,
        RegisterSignalSender(
            _, A<std::unique_ptr<
                   base::RepeatingCallback<bool(const ProvisionStatus&)>>>()))
        .WillRepeatedly(Return());
    EXPECT_CALL(
        mock_rmad_service_,
        RegisterSignalSender(
            _, A<std::unique_ptr<
                   base::RepeatingCallback<bool(const FinalizeStatus&)>>>()))
        .WillRepeatedly(Return());
  }
  ~DBusServiceTest() override = default;

  void SetUpDBusService(bool state_file_exist,
                        RoVerificationStatus ro_verification_status,
                        bool setup_success) {
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    base::FilePath state_file_path = temp_dir_.GetPath().AppendASCII("state");
    if (state_file_exist) {
      brillo::TouchFile(state_file_path);
    }
    auto mock_tpm_manager_client =
        std::make_unique<NiceMock<MockTpmManagerClient>>();
    ON_CALL(*mock_tpm_manager_client, GetRoVerificationStatus(_))
        .WillByDefault(
            DoAll(SetArgPointee<0>(ro_verification_status), Return(true)));
    dbus_service_ = std::make_unique<DBusService>(
        mock_bus_, &mock_rmad_service_, state_file_path,
        std::move(mock_tpm_manager_client));
    ASSERT_EQ(dbus_service_->OnEventLoopStarted(), EX_OK);

    auto sequencer = base::MakeRefCounted<AsyncEventSequencer>();
    dbus_service_->RegisterDBusObjectsAsync(sequencer.get());

    if (state_file_exist ||
        ro_verification_status == RoVerificationStatus::PASS) {
      EXPECT_CALL(mock_rmad_service_, SetUp())
          .WillRepeatedly(Return(setup_success));
      EXPECT_CALL(mock_rmad_service_, TryTransitionNextStateFromCurrentState())
          .WillRepeatedly(Return());
    }
  }

  template <typename RequestProtobufType, typename ReplyProtobufType>
  void ExecuteMethod(const std::string& method_name,
                     const RequestProtobufType& request,
                     ReplyProtobufType* reply) {
    std::unique_ptr<dbus::MethodCall> call = CreateMethodCall(method_name);
    dbus::MessageWriter writer(call.get());
    writer.AppendProtoAsArrayOfBytes(request);
    auto response = brillo::dbus_utils::testing::CallMethod(
        *dbus_service_->dbus_object_, call.get());
    if (response.get()) {
      dbus::MessageReader reader(response.get());
      EXPECT_TRUE(reader.PopArrayOfBytesAsProto(reply));
    }
  }

  template <typename ReplyProtobufType>
  void ExecuteMethod(const std::string& method_name, ReplyProtobufType* reply) {
    std::unique_ptr<dbus::MethodCall> call = CreateMethodCall(method_name);
    auto response = brillo::dbus_utils::testing::CallMethod(
        *dbus_service_->dbus_object_, call.get());
    if (response.get()) {
      dbus::MessageReader reader(response.get());
      EXPECT_TRUE(reader.PopArrayOfBytesAsProto(reply));
    }
  }

  void ExecuteMethod(const std::string& method_name, std::string* reply) {
    std::unique_ptr<dbus::MethodCall> call = CreateMethodCall(method_name);
    auto response = brillo::dbus_utils::testing::CallMethod(
        *dbus_service_->dbus_object_, call.get());
    if (response.get()) {
      dbus::MessageReader reader(response.get());
      EXPECT_TRUE(reader.PopString(reply));
    }
  }

  bool SignalError(RmadErrorCode error) {
    return dbus_service_->SendErrorSignal(error);
  }

  bool SignalHardwareVerification(const HardwareVerificationResult& result) {
    return dbus_service_->SendHardwareVerificationResultSignal(result);
  }

  bool SignalCalibrationOverall(CalibrationOverallStatus overall_status) {
    return dbus_service_->SendCalibrationOverallSignal(overall_status);
  }

  bool SignalCalibrationComponent(CalibrationComponentStatus component_status) {
    return dbus_service_->SendCalibrationProgressSignal(component_status);
  }

  bool SignalProvision(const ProvisionStatus& status) {
    return dbus_service_->SendProvisionProgressSignal(status);
  }

  bool SignalFinalize(const FinalizeStatus& status) {
    return dbus_service_->SendFinalizeProgressSignal(status);
  }

  bool SignalHardwareWriteProtection(bool enabled) {
    return dbus_service_->SendHardwareWriteProtectionStateSignal(enabled);
  }

  bool SignalPowerCable(bool plugged_in) {
    return dbus_service_->SendPowerCableStateSignal(plugged_in);
  }

  dbus::MockExportedObject* GetMockExportedObject() {
    return mock_exported_object_.get();
  }

 protected:
  std::unique_ptr<dbus::MethodCall> CreateMethodCall(
      const std::string& method_name) {
    auto call =
        std::make_unique<dbus::MethodCall>(kRmadInterfaceName, method_name);
    call->SetSerial(1);
    return call;
  }

  scoped_refptr<dbus::MockBus> mock_bus_;
  scoped_refptr<dbus::MockExportedObject> mock_exported_object_;
  base::ScopedTempDir temp_dir_;
  StrictMock<MockRmadInterface> mock_rmad_service_;
  std::unique_ptr<DBusService> dbus_service_;
};

TEST_F(DBusServiceTest, GetCurrentStats_RmaNotRequired) {
  SetUpDBusService(false, RoVerificationStatus::NOT_TRIGGERED, true);
  EXPECT_CALL(mock_rmad_service_, GetCurrentState(_))
      .WillOnce(Invoke([](const RmadInterface::GetStateCallback& callback) {
        GetStateReply reply;
        reply.set_error(RMAD_ERROR_RMA_NOT_REQUIRED);
        callback.Run(reply);
      }));
  GetStateReply reply;
  ExecuteMethod(kGetCurrentStateMethod, &reply);
  EXPECT_EQ(RMAD_ERROR_RMA_NOT_REQUIRED, reply.error());
  EXPECT_EQ(RmadState::STATE_NOT_SET, reply.state().state_case());
}

TEST_F(DBusServiceTest, GetCurrentState_RoVerificationTriggered) {
  SetUpDBusService(false, RoVerificationStatus::PASS, true);
  EXPECT_CALL(mock_rmad_service_, GetCurrentState(_))
      .WillOnce(Invoke([](const RmadInterface::GetStateCallback& callback) {
        GetStateReply reply;
        reply.set_error(RMAD_ERROR_RMA_NOT_REQUIRED);
        callback.Run(reply);
      }));

  GetStateReply reply;
  ExecuteMethod(kGetCurrentStateMethod, &reply);
  EXPECT_EQ(RMAD_ERROR_RMA_NOT_REQUIRED, reply.error());
  EXPECT_EQ(RmadState::STATE_NOT_SET, reply.state().state_case());
}

TEST_F(DBusServiceTest, GetCurrentState_StateFileExists) {
  SetUpDBusService(true, RoVerificationStatus::NOT_TRIGGERED, true);
  EXPECT_CALL(mock_rmad_service_, GetCurrentState(_))
      .WillOnce(Invoke([](const RmadInterface::GetStateCallback& callback) {
        GetStateReply reply;
        reply.set_error(RMAD_ERROR_RMA_NOT_REQUIRED);
        callback.Run(reply);
      }));

  GetStateReply reply;
  ExecuteMethod(kGetCurrentStateMethod, &reply);
  EXPECT_EQ(RMAD_ERROR_RMA_NOT_REQUIRED, reply.error());
  EXPECT_EQ(RmadState::STATE_NOT_SET, reply.state().state_case());
}

TEST_F(DBusServiceTest, GetCurrentState_InterfaceSetUpFailed) {
  SetUpDBusService(true, RoVerificationStatus::NOT_TRIGGERED, false);
  EXPECT_CALL(*GetMockExportedObject(), SendSignal(_))
      .WillRepeatedly(Invoke([](dbus::Signal* signal) {
        EXPECT_EQ(signal->GetInterface(), "org.chromium.Rmad");
        EXPECT_EQ(signal->GetMember(), "Error");
        dbus::MessageReader reader(signal);
        int error;
        EXPECT_TRUE(brillo::dbus_utils::DBusType<int>::Read(&reader, &error));
        EXPECT_EQ(static_cast<RmadErrorCode>(error),
                  RMAD_ERROR_DAEMON_INITIALIZATION_FAILED);
      }));

  GetStateReply reply;
  ExecuteMethod(kGetCurrentStateMethod, &reply);
}

TEST_F(DBusServiceTest, TransitionNextState) {
  SetUpDBusService(true, RoVerificationStatus::NOT_TRIGGERED, true);
  EXPECT_CALL(mock_rmad_service_, TransitionNextState(_, _))
      .WillOnce(Invoke([](const TransitionNextStateRequest& request,
                          const RmadInterface::GetStateCallback& callback) {
        GetStateReply reply;
        reply.set_error(RMAD_ERROR_OK);
        RmadState* state = new RmadState();
        state->set_allocated_welcome(new WelcomeState());
        reply.set_allocated_state(state);
        callback.Run(reply);
      }));

  TransitionNextStateRequest request;
  GetStateReply reply;
  ExecuteMethod(kTransitionNextStateMethod, request, &reply);
  EXPECT_EQ(RMAD_ERROR_OK, reply.error());
  EXPECT_EQ(RmadState::kWelcome, reply.state().state_case());
}

TEST_F(DBusServiceTest, TransitionPreviousState) {
  SetUpDBusService(true, RoVerificationStatus::NOT_TRIGGERED, true);
  EXPECT_CALL(mock_rmad_service_, TransitionPreviousState(_))
      .WillOnce(Invoke([](const RmadInterface::GetStateCallback& callback) {
        GetStateReply reply;
        reply.set_error(RMAD_ERROR_TRANSITION_FAILED);
        callback.Run(reply);
      }));

  GetStateReply reply;
  ExecuteMethod(kTransitionPreviousStateMethod, &reply);
  EXPECT_EQ(RMAD_ERROR_TRANSITION_FAILED, reply.error());
  EXPECT_EQ(RmadState::STATE_NOT_SET, reply.state().state_case());
}

TEST_F(DBusServiceTest, AbortRma) {
  SetUpDBusService(true, RoVerificationStatus::NOT_TRIGGERED, true);
  EXPECT_CALL(mock_rmad_service_, AbortRma(_))
      .WillOnce(Invoke([](const RmadInterface::AbortRmaCallback& callback) {
        AbortRmaReply reply;
        reply.set_error(RMAD_ERROR_ABORT_FAILED);
        callback.Run(reply);
      }));

  AbortRmaReply reply;
  ExecuteMethod(kAbortRmaMethod, &reply);
  EXPECT_EQ(RMAD_ERROR_ABORT_FAILED, reply.error());
}

TEST_F(DBusServiceTest, GetLogPath) {
  // This method doesn't call |mock_rma_service_|.
  SetUpDBusService(true, RoVerificationStatus::NOT_TRIGGERED, true);
  std::string reply;
  ExecuteMethod(kGetLogPathMethod, &reply);
  EXPECT_EQ("not_supported", reply);
}

TEST_F(DBusServiceTest, SignalError) {
  SetUpDBusService(true, RoVerificationStatus::NOT_TRIGGERED, true);
  EXPECT_CALL(*GetMockExportedObject(), SendSignal(_))
      .WillRepeatedly(Invoke([](dbus::Signal* signal) {
        EXPECT_EQ(signal->GetInterface(), "org.chromium.Rmad");
        EXPECT_EQ(signal->GetMember(), "Error");
        dbus::MessageReader reader(signal);
        int error;
        EXPECT_TRUE(reader.PopInt32(&error));
        EXPECT_EQ(error, RMAD_ERROR_RMA_NOT_REQUIRED);
      }));
  EXPECT_TRUE(SignalError(RMAD_ERROR_RMA_NOT_REQUIRED));
}

TEST_F(DBusServiceTest, SignalHardwareVerification) {
  SetUpDBusService(true, RoVerificationStatus::NOT_TRIGGERED, true);
  EXPECT_CALL(*GetMockExportedObject(), SendSignal(_))
      .WillRepeatedly(Invoke([](dbus::Signal* signal) {
        EXPECT_EQ(signal->GetInterface(), "org.chromium.Rmad");
        EXPECT_EQ(signal->GetMember(), "HardwareVerificationResult");
        dbus::MessageReader reader(signal);
        HardwareVerificationResult result;
        EXPECT_TRUE(PopValueFromReader(&reader, &result));
        EXPECT_EQ(result.is_compliant(), true);
        EXPECT_EQ(result.error_str(), "test_error_string");
      }));
  HardwareVerificationResult result;
  result.set_is_compliant(true);
  result.set_error_str("test_error_string");
  EXPECT_TRUE(SignalHardwareVerification(result));
}

TEST_F(DBusServiceTest, SignalCalibrationOverall) {
  SetUpDBusService(true, RoVerificationStatus::NOT_TRIGGERED, true);
  EXPECT_CALL(*GetMockExportedObject(), SendSignal(_))
      .WillRepeatedly(Invoke([](dbus::Signal* signal) {
        EXPECT_EQ(signal->GetInterface(), "org.chromium.Rmad");
        EXPECT_EQ(signal->GetMember(), "CalibrationOverall");
        dbus::MessageReader reader(signal);
        int overall_status;
        EXPECT_TRUE(reader.PopInt32(&overall_status));
        EXPECT_EQ(overall_status,
                  RMAD_CALIBRATION_OVERALL_CURRENT_ROUND_COMPLETE);
      }));
  EXPECT_TRUE(SignalCalibrationOverall(
      RMAD_CALIBRATION_OVERALL_CURRENT_ROUND_COMPLETE));
}

TEST_F(DBusServiceTest, SignalCalibrationComponent) {
  SetUpDBusService(true, RoVerificationStatus::NOT_TRIGGERED, true);
  EXPECT_CALL(*GetMockExportedObject(), SendSignal(_))
      .WillRepeatedly(Invoke([](dbus::Signal* signal) {
        EXPECT_EQ(signal->GetInterface(), "org.chromium.Rmad");
        EXPECT_EQ(signal->GetMember(), "CalibrationProgress");
        dbus::MessageReader reader(signal);
        CalibrationComponentStatus calibration_status;
        EXPECT_TRUE(PopValueFromReader(&reader, &calibration_status));
        EXPECT_EQ(calibration_status.component(),
                  RmadComponent::RMAD_COMPONENT_BASE_ACCELEROMETER);
        EXPECT_EQ(calibration_status.status(),
                  CalibrationComponentStatus::RMAD_CALIBRATION_IN_PROGRESS);
        EXPECT_EQ(calibration_status.progress(), 0.3);
      }));
  CalibrationComponentStatus component_status;
  component_status.set_component(
      RmadComponent::RMAD_COMPONENT_BASE_ACCELEROMETER);
  component_status.set_status(
      CalibrationComponentStatus::RMAD_CALIBRATION_IN_PROGRESS);
  component_status.set_progress(0.3);
  EXPECT_TRUE(SignalCalibrationComponent(component_status));
}

TEST_F(DBusServiceTest, SignalProvision) {
  SetUpDBusService(true, RoVerificationStatus::NOT_TRIGGERED, true);
  EXPECT_CALL(*GetMockExportedObject(), SendSignal(_))
      .WillRepeatedly(Invoke([](dbus::Signal* signal) {
        EXPECT_EQ(signal->GetInterface(), "org.chromium.Rmad");
        EXPECT_EQ(signal->GetMember(), "ProvisioningProgress");
        dbus::MessageReader reader(signal);
        ProvisionStatus status;
        EXPECT_TRUE(PopValueFromReader(&reader, &status));
        EXPECT_EQ(status.status(),
                  ProvisionStatus::RMAD_PROVISION_STATUS_IN_PROGRESS);
        EXPECT_EQ(status.progress(), 0.5);
      }));
  ProvisionStatus status;
  status.set_status(ProvisionStatus::RMAD_PROVISION_STATUS_IN_PROGRESS);
  status.set_progress(0.5);
  EXPECT_TRUE(SignalProvision(status));
}

TEST_F(DBusServiceTest, SignalFinalize) {
  SetUpDBusService(true, RoVerificationStatus::NOT_TRIGGERED, true);
  EXPECT_CALL(*GetMockExportedObject(), SendSignal(_))
      .WillRepeatedly(Invoke([](dbus::Signal* signal) {
        EXPECT_EQ(signal->GetInterface(), "org.chromium.Rmad");
        EXPECT_EQ(signal->GetMember(), "FinalizeProgress");
        dbus::MessageReader reader(signal);
        FinalizeStatus status;
        EXPECT_TRUE(PopValueFromReader(&reader, &status));
        EXPECT_EQ(status.status(),
                  FinalizeStatus::RMAD_FINALIZE_STATUS_IN_PROGRESS);
        EXPECT_EQ(status.progress(), 0.5);
      }));
  FinalizeStatus status;
  status.set_status(FinalizeStatus::RMAD_FINALIZE_STATUS_IN_PROGRESS);
  status.set_progress(0.5);
  EXPECT_TRUE(SignalFinalize(status));
}

TEST_F(DBusServiceTest, SignalHardwareWriteProtection) {
  SetUpDBusService(true, RoVerificationStatus::NOT_TRIGGERED, true);
  EXPECT_CALL(*GetMockExportedObject(), SendSignal(_))
      .WillRepeatedly(Invoke([](dbus::Signal* signal) {
        EXPECT_EQ(signal->GetInterface(), "org.chromium.Rmad");
        EXPECT_EQ(signal->GetMember(), "HardwareWriteProtectionState");
        dbus::MessageReader reader(signal);
        bool enabled;
        EXPECT_TRUE(reader.PopBool(&enabled));
        EXPECT_TRUE(enabled);
      }));
  EXPECT_TRUE(SignalHardwareWriteProtection(true));
}

TEST_F(DBusServiceTest, SignalPowerCable) {
  SetUpDBusService(true, RoVerificationStatus::NOT_TRIGGERED, true);
  EXPECT_CALL(*GetMockExportedObject(), SendSignal(_))
      .WillRepeatedly(Invoke([](dbus::Signal* signal) {
        EXPECT_EQ(signal->GetInterface(), "org.chromium.Rmad");
        EXPECT_EQ(signal->GetMember(), "PowerCableState");
        dbus::MessageReader reader(signal);
        bool plugged_in;
        EXPECT_TRUE(reader.PopBool(&plugged_in));
        EXPECT_TRUE(plugged_in);
      }));
  EXPECT_TRUE(SignalPowerCable(true));
}

}  // namespace rmad
