// Copyright 2020 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <base/callback.h>
#include <base/check.h>
#include <base/logging.h>
#include <brillo/errors/error_codes.h>
#include <chromeos/dbus/service_constants.h>
#include <google-lpa/lpa/core/lpa.h>
#include <google-lpa/lpa/data/proto/euicc_info_1.pb.h>

#include "hermes/euicc.h"
#include "hermes/euicc_cache.h"
#include "hermes/executor.h"
#include "hermes/hermes_common.h"
#include "hermes/lpa_util.h"

using lpa::proto::ProfileInfo;

namespace hermes {

namespace {

const char kDefaultProdRootSmds[] = "lpa.ds.gsma.com";
const char kDefaultTestRootSmds[] = "testrootsmds.example.com";

void PrintEuiccEventResult(int err) {
  if (err) {
    LOG(ERROR) << "ProcessEuiccEvent failed with err=" << err;
    return;
  }
  VLOG(2) << "ProcessEuiccEvent succeeded";
}

}  // namespace

Euicc::Euicc(uint8_t physical_slot, EuiccSlotInfo slot_info)
    : physical_slot_(physical_slot),
      slot_info_(std::move(slot_info)),
      is_test_mode_(false),
      use_test_certs_(false),
      context_(Context::Get()),
      dbus_adaptor_(context_->adaptor_factory()->CreateEuiccAdaptor(this)),
      weak_factory_(this) {
  dbus_adaptor_->SetPendingProfiles({});
  dbus_adaptor_->SetPhysicalSlot(physical_slot_);
  UpdateSlotInfo(slot_info_);
}

void Euicc::UpdateSlotInfo(EuiccSlotInfo slot_info) {
  slot_info_ = std::move(slot_info);
  dbus_adaptor_->SetEid(slot_info_.eid_);
  dbus_adaptor_->SetIsActive(slot_info_.IsActive());
}

void Euicc::UpdateLogicalSlot(std::optional<uint8_t> logical_slot) {
  slot_info_.SetLogicalSlot(std::move(logical_slot));
  dbus_adaptor_->SetIsActive(slot_info_.IsActive());
}

void Euicc::InstallProfileFromActivationCode(
    std::string activation_code,
    std::string confirmation_code,
    DbusResult<dbus::ObjectPath> dbus_result) {
  LOG(INFO) << __func__;
  if (!context_->lpa()->IsLpaIdle()) {
    // The LPA performs background tasks even after a dbus call is returned.
    // During this period(about 2 seconds), we must not perform any operations
    // that could disrupt the state of the transmit queue (slot-switching,
    // acquiring a new channel etc.).
    context_->executor()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&Euicc::InstallProfileFromActivationCode,
                       weak_factory_.GetWeakPtr(), std::move(activation_code),
                       std::move(confirmation_code), std::move(dbus_result)),
        kLpaRetryDelay);
    return;
  }
  auto download_profile =
      base::BindOnce(&Euicc::DownloadProfile, weak_factory_.GetWeakPtr(),
                     std::move(activation_code), std::move(confirmation_code));
  auto get_card_version =
      base::BindOnce(&Euicc::GetCardVersion<dbus::ObjectPath>,
                     weak_factory_.GetWeakPtr(), std::move(download_profile));
  context_->modem_control()->ProcessEuiccEvent(
      {physical_slot_, EuiccStep::START},
      base::BindOnce(&Euicc::RunOnSuccess<dbus::ObjectPath>,
                     weak_factory_.GetWeakPtr(), std::move(get_card_version),
                     std::move(dbus_result)));
}

void Euicc::DownloadProfile(std::string activation_code,
                            std::string confirmation_code,
                            DbusResult<dbus::ObjectPath> dbus_result) {
  LOG(INFO) << __func__;
  auto profile_cb = [dbus_result{std::move(dbus_result)}, this](
                        lpa::proto::ProfileInfo& info, int error) mutable {
    OnProfileInstalled(info, error, std::move(dbus_result));
  };
  if (activation_code.empty()) {
    context_->lpa()->GetDefaultProfileFromSmdp("", context_->executor(),
                                               std::move(profile_cb));
    return;
  }

  lpa::core::Lpa::DownloadOptions options;
  options.enable_profile = false;
  options.allow_policy_rules = false;
  options.confirmation_code = confirmation_code;
  context_->lpa()->DownloadProfile(activation_code, std::move(options),
                                   context_->executor(), std::move(profile_cb));
}

void Euicc::InstallPendingProfile(dbus::ObjectPath profile_path,
                                  std::string confirmation_code,
                                  DbusResult<dbus::ObjectPath> dbus_result) {
  LOG(INFO) << __func__ << " " << GetObjectPathForLog(profile_path);
  if (!context_->lpa()->IsLpaIdle()) {
    context_->executor()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&Euicc::InstallPendingProfile,
                       weak_factory_.GetWeakPtr(), std::move(profile_path),
                       std::move(confirmation_code), std::move(dbus_result)),
        kLpaRetryDelay);
    return;
  }
  auto iter = find_if(pending_profiles_.begin(), pending_profiles_.end(),
                      [&profile_path](const std::unique_ptr<Profile>& profile) {
                        return profile->object_path() == profile_path;
                      });

  if (iter == pending_profiles_.end()) {
    dbus_result.Error(brillo::Error::Create(
        FROM_HERE, brillo::errors::dbus::kDomain, kErrorInvalidParameter,
        "Could not find Profile " + profile_path.value()));
    return;
  }

  std::string activation_code = iter->get()->GetActivationCode();
  InstallProfileFromActivationCode(std::move(activation_code),
                                   std::move(confirmation_code),
                                   std::move(dbus_result));
}

void Euicc::UninstallProfile(dbus::ObjectPath profile_path,
                             DbusResult<> dbus_result) {
  LOG(INFO) << __func__ << " " << GetObjectPathForLog(profile_path);
  if (!context_->lpa()->IsLpaIdle()) {
    context_->executor()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&Euicc::UninstallProfile, weak_factory_.GetWeakPtr(),
                       std::move(profile_path), std::move(dbus_result)),
        kLpaRetryDelay);
    return;
  }
  const Profile* matching_profile = nullptr;
  for (auto& profile : installed_profiles_) {
    if (profile->object_path() == profile_path) {
      matching_profile = profile.get();
      break;
    }
  }
  if (!matching_profile) {
    dbus_result.Error(brillo::Error::Create(
        FROM_HERE, brillo::errors::dbus::kDomain, kErrorInvalidParameter,
        "Could not find Profile " + profile_path.value()));
    return;
  }

  auto delete_profile =
      base::BindOnce(&Euicc::DeleteProfile, weak_factory_.GetWeakPtr(),
                     std::move(profile_path), matching_profile->GetIccid());
  context_->modem_control()->ProcessEuiccEvent(
      {physical_slot_, EuiccStep::START},
      base::BindOnce(&Euicc::RunOnSuccess<>, weak_factory_.GetWeakPtr(),
                     std::move(delete_profile), std::move(dbus_result)));
}

void Euicc::DeleteProfile(dbus::ObjectPath profile_path,
                          std::string iccid,
                          DbusResult<> dbus_result) {
  context_->lpa()->DeleteProfile(std::move(iccid), context_->executor(),
                                 [dbus_result{std::move(dbus_result)},
                                  profile_path, this](int error) mutable {
                                   OnProfileUninstalled(profile_path, error,
                                                        std::move(dbus_result));
                                 });
}

void Euicc::UpdateInstalledProfilesProperty() {
  std::vector<dbus::ObjectPath> profile_paths;
  LOG(INFO) << __func__;
  for (auto& profile : installed_profiles_) {
    profile_paths.push_back(profile->object_path());
  }
  dbus_adaptor_->SetInstalledProfiles(profile_paths);
}

void Euicc::UpdatePendingProfilesProperty() {
  LOG(INFO) << __func__;
  std::vector<dbus::ObjectPath> profile_paths;
  for (auto& profile : pending_profiles_) {
    profile_paths.push_back(profile->object_path());
  }
  dbus_adaptor_->SetPendingProfiles(profile_paths);
}

void Euicc::OnProfileInstalled(const lpa::proto::ProfileInfo& profile_info,
                               int error,
                               DbusResult<dbus::ObjectPath> dbus_result) {
  LOG(INFO) << __func__;
  auto decoded_error = LpaErrorToBrillo(FROM_HERE, error);
  if (decoded_error) {
    EndEuiccOp(dbus_result, std::move(decoded_error));
    return;
  }

  auto iter = find_if(pending_profiles_.begin(), pending_profiles_.end(),
                      [&profile_info](const std::unique_ptr<Profile>& profile) {
                        return profile->GetIccid() == profile_info.iccid();
                      });

  std::unique_ptr<Profile> profile;
  // Call UpdatePendingProfilesProperty() after UpdateInstalledProfilesProperty,
  // else Chrome assumes the pending profile was deleted forever.
  bool update_pending_profiles_property = false;
  if (iter != pending_profiles_.end()) {
    // Remove the profile from pending_profiles_ so that it can become an
    // installed profile
    profile = std::move(*iter);
    pending_profiles_.erase(iter);
    update_pending_profiles_property = true;
  } else {
    profile = Profile::Create(profile_info, physical_slot_, slot_info_.eid_,
                              /*is_pending*/ false,
                              base::BindRepeating(&Euicc::OnProfileEnabled,
                                                  weak_factory_.GetWeakPtr()));
  }

  if (!profile) {
    auto profile_error = brillo::Error::Create(
        FROM_HERE, brillo::errors::dbus::kDomain, kErrorInternalLpaFailure,
        "Failed to create Profile object");
    EndEuiccOp(dbus_result, std::move(profile_error));
    return;
  }

  installed_profiles_.push_back(std::move(profile));
  UpdateInstalledProfilesProperty();
  if (update_pending_profiles_property) {
    UpdatePendingProfilesProperty();
    installed_profiles_.back()->SetState(profile::kInactive);
  }
  // Refresh LPA profile cache
  // Send notifications and refresh LPA profile cache. No errors will be raised
  // if these operations fail since the profile installation already succeeded.
  context_->lpa()->SendNotifications(
      context_->executor(),
      [this, dbus_result{std::move(dbus_result)},
       profile_path{installed_profiles_.back()->object_path()}](int /*err*/) {
        // Send notifications has completed, refresh the profile cache.
        context_->lpa()->GetInstalledProfiles(
            context_->executor(),
            [this, dbus_result{std::move(dbus_result)}, profile_path](
                std::vector<lpa::proto::ProfileInfo>& profile_infos,
                int /*error*/) { EndEuiccOp(dbus_result, profile_path); });
      });
}

void Euicc::OnProfileEnabled(const std::string& iccid) {
  for (auto& installed_profile : installed_profiles_) {
    installed_profile->SetState(installed_profile->GetIccid() == iccid
                                    ? profile::kActive
                                    : profile::kInactive);
  }
}

void Euicc::OnProfileUninstalled(const dbus::ObjectPath& profile_path,
                                 int error,
                                 DbusResult<> dbus_result) {
  LOG(INFO) << __func__;
  auto decoded_error = LpaErrorToBrillo(FROM_HERE, error);
  if (decoded_error) {
    EndEuiccOp(dbus_result, std::move(decoded_error));
    return;
  }

  auto iter = installed_profiles_.begin();
  for (; iter != installed_profiles_.end(); ++iter) {
    if ((*iter)->object_path() == profile_path) {
      break;
    }
  }
  CHECK(iter != installed_profiles_.end());
  installed_profiles_.erase(iter);
  UpdateInstalledProfilesProperty();
  SendNotifications(std::move(dbus_result));
}
void Euicc::SendNotifications(
    DbusResult<> dbus_result) {  // Send notifications and refresh LPA profile
                                 // cache. No errors will be raised
  // if these operations fail since the profile operation already succeeded.
  context_->lpa()->SendNotifications(
      context_->executor(),
      [this, dbus_result{std::move(dbus_result)}](int /*err*/) {
        context_->lpa()->GetInstalledProfiles(
            context_->executor(),
            [this, dbus_result{std::move(dbus_result)}](
                std::vector<lpa::proto::ProfileInfo>& profile_infos,
                int /*error*/) { EndEuiccOp(dbus_result); });
      });
}

void Euicc::RefreshInstalledProfiles(bool restore_slot,
                                     DbusResult<> dbus_result) {
  LOG(INFO) << __func__ << ": restore_slot=" << restore_slot;
  if (!context_->lpa()->IsLpaIdle()) {
    context_->executor()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&Euicc::RefreshInstalledProfiles,
                       weak_factory_.GetWeakPtr(), restore_slot,
                       std::move(dbus_result)),
        kLpaRetryDelay);
    return;
  }
  auto get_installed_profiles = base::BindOnce(
      &Euicc::GetInstalledProfiles, weak_factory_.GetWeakPtr(), restore_slot);
  auto get_card_version =
      base::BindOnce(&Euicc::GetCardVersion<>, weak_factory_.GetWeakPtr(),
                     std::move(get_installed_profiles));
  context_->modem_control()->ProcessEuiccEvent(
      {physical_slot_, EuiccStep::START},
      base::BindOnce(&Euicc::RunOnSuccess<>, weak_factory_.GetWeakPtr(),
                     std::move(get_card_version), std::move(dbus_result)));
}

void Euicc::GetInstalledProfiles(bool restore_slot, DbusResult<> dbus_result) {
  context_->lpa()->GetInstalledProfiles(
      context_->executor(),
      [restore_slot, dbus_result{std::move(dbus_result)}, this](
          std::vector<lpa::proto::ProfileInfo>& profile_infos,
          int error) mutable {
        OnInstalledProfilesReceived(profile_infos, error, restore_slot,
                                    std::move(dbus_result));
      });
}

void Euicc::OnInstalledProfilesReceived(
    const std::vector<lpa::proto::ProfileInfo>& profile_infos,
    int error,
    bool restore_slot,
    DbusResult<> dbus_result) {
  LOG(INFO) << __func__;
  auto decoded_error = LpaErrorToBrillo(FROM_HERE, error);
  if (decoded_error) {
    LOG(ERROR) << "Failed to retrieve installed profiles";
    EndEuiccOp(dbus_result, std::move(decoded_error));
    return;
  }
  installed_profiles_.clear();
  for (const auto& info : profile_infos) {
    if (!is_test_mode_ &&
        info.profile_class() == lpa::proto::ProfileClass::TESTING)
      continue;
    auto profile =
        Profile::Create(info, physical_slot_, slot_info_.eid_,
                        /*is_pending*/ false,
                        base::BindRepeating(&Euicc::OnProfileEnabled,
                                            weak_factory_.GetWeakPtr()));
    if (profile) {
      installed_profiles_.push_back(std::move(profile));
    }
  }
  UpdateInstalledProfilesProperty();
  if (!restore_slot) {
    EndEuiccOp(dbus_result);
    return;
  }
  // Restore the active slot and Run end_euicc_op.
  auto end_euicc_op =
      base::BindOnce(&Euicc::EndEuiccOpNoObject, weak_factory_.GetWeakPtr());
  context_->modem_control()->RestoreActiveSlot(
      base::BindOnce(&Euicc::RunOnSuccess<>, weak_factory_.GetWeakPtr(),
                     std::move(end_euicc_op), std::move(dbus_result)));
}

void Euicc::RequestPendingProfiles(DbusResult<> dbus_result,
                                   std::string root_smds) {
  LOG(INFO) << __func__;
  if (!context_->lpa()->IsLpaIdle()) {
    context_->executor()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&Euicc::RequestPendingProfiles,
                       weak_factory_.GetWeakPtr(), std::move(dbus_result),
                       std::move(root_smds)),
        kLpaRetryDelay);
    return;
  }
  auto get_pending_profiles_from_smds =
      base::BindOnce(&Euicc::GetPendingProfilesFromSmds,
                     weak_factory_.GetWeakPtr(), std::move(root_smds));
  auto get_card_version =
      base::BindOnce(&Euicc::GetCardVersion<>, weak_factory_.GetWeakPtr(),
                     std::move(get_pending_profiles_from_smds));
  context_->modem_control()->ProcessEuiccEvent(
      {physical_slot_, EuiccStep::START},
      base::BindOnce(&Euicc::RunOnSuccess<>, weak_factory_.GetWeakPtr(),
                     std::move(get_card_version), std::move(dbus_result)));
}

void Euicc::GetPendingProfilesFromSmds(std::string root_smds,
                                       DbusResult<> dbus_result) {
  auto default_smds =
      use_test_certs_ ? kDefaultTestRootSmds : kDefaultProdRootSmds;
  auto smds = root_smds.empty() ? default_smds : root_smds;
  context_->lpa()->GetPendingProfilesFromSmds(
      smds, context_->executor(),
      [dbus_result{std::move(dbus_result)}, this](
          std::vector<lpa::proto::ProfileInfo>& profile_infos,
          int error) mutable {
        OnPendingProfilesReceived(profile_infos, error, std::move(dbus_result));
      });
}

void Euicc::OnPendingProfilesReceived(
    const std::vector<lpa::proto::ProfileInfo>& profile_infos,
    int error,
    DbusResult<> dbus_result) {
  LOG(INFO) << __func__;
  auto decoded_error = LpaErrorToBrillo(FROM_HERE, error);
  if (decoded_error) {
    LOG(ERROR) << "Failed to retrieve pending profiles";
    EndEuiccOp(dbus_result, std::move(decoded_error));
    return;
  }

  pending_profiles_.clear();
  for (const auto& info : profile_infos) {
    auto profile =
        Profile::Create(info, physical_slot_, slot_info_.eid_,
                        /*is_pending*/ true,
                        base::BindRepeating(&Euicc::OnProfileEnabled,
                                            weak_factory_.GetWeakPtr()));
    if (profile) {
      pending_profiles_.push_back(std::move(profile));
    }
  }
  UpdatePendingProfilesProperty();
  EndEuiccOp(dbus_result);
}

void Euicc::SetTestModeHelper(bool is_test_mode, DbusResult<> dbus_result) {
  VLOG(2) << __func__ << " : is_test_mode" << is_test_mode;
  is_test_mode_ = is_test_mode;
  auto set_test_mode_internal = base::BindOnce(
      &Euicc::SetTestMode, weak_factory_.GetWeakPtr(), is_test_mode);
  context_->modem_control()->ProcessEuiccEvent(
      {physical_slot_, EuiccStep::START},
      base::BindOnce(&Euicc::RunOnSuccess<>, weak_factory_.GetWeakPtr(),
                     std::move(set_test_mode_internal),
                     std::move(dbus_result)));
}

void Euicc::SetTestMode(bool is_test_mode, DbusResult<> dbus_result) {
  VLOG(2) << __func__ << " : is_test_mode" << is_test_mode;
  context_->lpa()->SetTestMode(
      is_test_mode, context_->executor(),
      [this, dbus_result{std::move(dbus_result)}](int error) {
        auto decoded_error = LpaErrorToBrillo(FROM_HERE, error);
        if (decoded_error) {
          EndEuiccOp(dbus_result, std::move(decoded_error));
          return;
        }
        EndEuiccOp(dbus_result);
      });
}

void Euicc::UseTestCerts(bool use_test_certs) {
  const std::string kPath("/usr/share/hermes-ca-certificates/");
  // TODO(pholla): b/180422014 - all euicc's share the same LPA. Setting a euicc
  // to use test certs will make other euiccs use test certs too.
  context_->lpa()->SetTlsCertsDir(kPath + (use_test_certs ? "test/" : "prod/"));
  use_test_certs_ = use_test_certs;
}

void Euicc::ResetMemoryHelper(DbusResult<> dbus_result, int reset_options) {
  VLOG(2) << __func__ << " : reset_options: " << reset_options;
  if (reset_options != lpa::data::reset_options::kDeleteOperationalProfiles &&
      reset_options !=
          lpa::data::reset_options::kDeleteFieldLoadedTestProfiles) {
    dbus_result.Error(brillo::Error::Create(
        FROM_HERE, brillo::errors::dbus::kDomain, kErrorInvalidParameter,
        "Illegal value for reset_options."));
    return;
  }

  auto reset_memory_internal = base::BindOnce(
      &Euicc::ResetMemory, weak_factory_.GetWeakPtr(), reset_options);
  auto get_card_version =
      base::BindOnce(&Euicc::GetCardVersion<>, weak_factory_.GetWeakPtr(),
                     std::move(reset_memory_internal));
  context_->modem_control()->ProcessEuiccEvent(
      {physical_slot_, EuiccStep::START},
      base::BindOnce(&Euicc::RunOnSuccess<>, weak_factory_.GetWeakPtr(),
                     std::move(get_card_version), std::move(dbus_result)));
}

void Euicc::ResetMemory(int reset_options, DbusResult<> dbus_result) {
  bool reset_uicc = false;  // Ignored by the lpa.
  context_->lpa()->ResetMemory(
      reset_options, reset_uicc, context_->executor(),
      [this, dbus_result{std::move(dbus_result)}](int error) {
        auto decoded_error = LpaErrorToBrillo(FROM_HERE, error);
        if (decoded_error) {
          EndEuiccOp(dbus_result, std::move(decoded_error));
          return;
        }
        installed_profiles_.clear();
        UpdateInstalledProfilesProperty();
        SendNotifications(std::move(dbus_result));
      });
}

void Euicc::IsTestEuicc(DbusResult<bool> dbus_result) {
  LOG(INFO) << __func__;

  auto get_euicc_info_1 =
      base::BindOnce(&Euicc::GetEuiccInfo1, weak_factory_.GetWeakPtr());
  context_->modem_control()->ProcessEuiccEvent(
      {physical_slot_, EuiccStep::START},
      base::BindOnce(&Euicc::RunOnSuccess<bool>, weak_factory_.GetWeakPtr(),
                     std::move(get_euicc_info_1), std::move(dbus_result)));
}

void Euicc::GetEuiccInfo1(DbusResult<bool> dbus_result) {
  LOG(INFO) << __func__;

  context_->lpa()->GetEuiccInfo1(
      context_->executor(),
      [this, dbus_result{std::move(dbus_result)}](
          lpa::proto::EuiccInfo1& euicc_info_1, int error) {
        LOG(INFO) << "euicc_info_1:" << euicc_info_1.DebugString();
        auto decoded_error = LpaErrorToBrillo(FROM_HERE, error);
        if (decoded_error) {
          EndEuiccOp(dbus_result, std::move(decoded_error));
          return;
        }
        constexpr char kTestEuiccInfo1[] =
            "665A1433D67C1A2C5DB8B52C967F10A057BA5CB2";
        for (const auto& pkid : euicc_info_1.pkid_for_verif()) {
          if (pkid == kTestEuiccInfo1) {
            EndEuiccOp(dbus_result, true);
            return;
          }
        }
        EndEuiccOp(dbus_result, false);
      });
}

template <typename... T>
void Euicc::EndEuiccOp(DbusResult<T...> dbus_result, T... object) {
  auto send_dbus_response = base::BindOnce(
      [](const DbusResult<T...>& dbus_result, const T&... object, int err) {
        PrintEuiccEventResult(err);
        dbus_result.Success(object...);
      },
      dbus_result, object...);
  context_->modem_control()->ProcessEuiccEvent({physical_slot_, EuiccStep::END},
                                               std::move(send_dbus_response));
}

// Remove variadic template, and polymorphism on EndEuiccOp for use in lambdas
// and callbacks.
void Euicc::EndEuiccOpNoObject(DbusResult<> dbus_result) {
  EndEuiccOp(std::move(dbus_result));
}

template <typename... T>
void Euicc::EndEuiccOp(DbusResult<T...> dbus_result, brillo::ErrorPtr error) {
  auto send_dbus_response = base::BindOnce(
      [](const DbusResult<T...>& dbus_result, brillo::ErrorPtr error, int err) {
        PrintEuiccEventResult(err);
        dbus_result.Error(error);
      },
      dbus_result, std::move(error));
  context_->modem_control()->ProcessEuiccEvent({physical_slot_, EuiccStep::END},
                                               std::move(send_dbus_response));
}

template <typename... T>
void Euicc::RunOnSuccess(base::OnceCallback<void(DbusResult<T...>)> cb,
                         DbusResult<T...> dbus_result,
                         int err) {
  if (err) {
    LOG(ERROR) << "Received modem error: " << err;
    auto decoded_error = brillo::Error::Create(
        FROM_HERE, brillo::errors::dbus::kDomain, kErrorUnknown,
        "QMI/MBIM operation failed with code: " + std::to_string(err));
    EndEuiccOp(dbus_result, std::move(decoded_error));
    return;
  }
  std::move(cb).Run(std::move(dbus_result));
}

template <typename... T>
void Euicc::GetCardVersion(base::OnceCallback<void(DbusResult<T...>)> next_step,
                           DbusResult<T...> dbus_result) {
  // convert next_step into a copyable type (repeating callback), so that it can
  // be captured in a lambda.
  auto copyable_next_step = base::BindRepeating(
      [](base::OnceCallback<void(DbusResult<T...>)> next_step,
         DbusResult<T...> dbus_result) {
        std::move(next_step).Run(std::move(dbus_result));
      },
      base::Passed(std::move(next_step)));
  context_->lpa()->GetEuiccInfo1(
      context_->executor(),
      [this, dbus_result{std::move(dbus_result)}, copyable_next_step](
          lpa::proto::EuiccInfo1& euicc_info_1, int error) {
        LOG(INFO) << "euicc_info_1:" << euicc_info_1.DebugString();
        auto decoded_error = LpaErrorToBrillo(FROM_HERE, error);
        if (decoded_error) {
          EndEuiccOp(dbus_result, std::move(decoded_error));
          return;
        }
        context_->modem_control()->SetCardVersion(
            euicc_info_1.euicc_spec_version());
        copyable_next_step.Run(std::move(dbus_result));
      });
}

}  // namespace hermes
