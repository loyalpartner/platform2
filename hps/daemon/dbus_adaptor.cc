// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "hps/daemon/dbus_adaptor.h"

#include <utility>
#include <vector>

#include <base/location.h>
#include <brillo/errors/error.h>
#include <brillo/errors/error_codes.h>
#include <chromeos/dbus/service_constants.h>
#include <hps/daemon/filters/filter_factory.h>

namespace hps {

constexpr char kErrorPath[] = "org.chromium.Hps.GetFeatureResultError";

namespace {

std::vector<uint8_t> HpsResultToSerializedBytes(HpsResult result) {
  HpsResultProto result_proto;
  result_proto.set_value(result);

  std::vector<uint8_t> serialized;
  serialized.resize(result_proto.ByteSizeLong());
  result_proto.SerializeToArray(serialized.data(),
                                static_cast<int>(serialized.size()));
  return serialized;
}

}  // namespace

DBusAdaptor::DBusAdaptor(scoped_refptr<dbus::Bus> bus,
                         std::unique_ptr<HPS> hps,
                         uint32_t poll_time_ms)
    : org::chromium::HpsAdaptor(this),
      dbus_object_(nullptr, bus, dbus::ObjectPath(::hps::kHpsServicePath)),
      hps_(std::move(hps)),
      poll_time_ms_(poll_time_ms) {
  ShutDown();
}

void DBusAdaptor::RegisterAsync(
    const brillo::dbus_utils::AsyncEventSequencer::CompletionAction& cb) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  RegisterWithDBusObject(&dbus_object_);
  dbus_object_.RegisterAsync(cb);
}

void DBusAdaptor::PollTask() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  for (uint8_t feature = 0; feature < kFeatures; ++feature) {
    if (enabled_features_.test(feature)) {
      FeatureResult result = this->hps_->Result(feature);
      DCHECK(feature_filters_[feature]);
      const auto res = feature_filters_[feature]->ProcessResult(
          result.inference_result, result.valid);
      VLOG(2) << "Poll: Feature: " << static_cast<int>(feature)
              << " Valid: " << result.valid
              << " Result: " << static_cast<int>(result.inference_result)
              << " Filter: " << static_cast<int>(res);
    }
  }
}

void DBusAdaptor::BootIfNeeded() {
  if (hps_booted_) {
    return;
  }
  if (!hps_->Boot()) {
    LOG(FATAL) << "Failed to boot";
  }
  hps_booted_ = true;
}

void DBusAdaptor::ShutDown() {
  DCHECK(!poll_timer_.IsRunning());
  if (!hps_->ShutDown()) {
    LOG(FATAL) << "Failed to shutdown";
  }
  hps_booted_ = false;
}

bool DBusAdaptor::EnableFeature(brillo::ErrorPtr* error,
                                const hps::FeatureConfig& config,
                                uint8_t feature,
                                StatusCallback callback) {
  BootIfNeeded();
  if (!this->hps_->Enable(feature)) {
    brillo::Error::AddTo(error, FROM_HERE, brillo::errors::dbus::kDomain,
                         kErrorPath, "hpsd: Unable to enable feature");
    if (enabled_features_.none()) {
      ShutDown();
    }
    return false;
  } else {
    auto filter = CreateFilter(config, callback);
    feature_filters_[feature] = std::move(filter);
    enabled_features_.set(feature);

    if (enabled_features_.any() && !poll_timer_.IsRunning()) {
      poll_timer_.Start(
          FROM_HERE, base::TimeDelta::FromMilliseconds(poll_time_ms_),
          base::BindRepeating(&DBusAdaptor::PollTask, base::Unretained(this)));
    }
    return true;
  }
}

bool DBusAdaptor::DisableFeature(brillo::ErrorPtr* error, uint8_t feature) {
  if (!this->hps_->Disable(feature)) {
    brillo::Error::AddTo(error, FROM_HERE, brillo::errors::dbus::kDomain,
                         kErrorPath, "hpsd: Unable to disable feature");
    return false;
  } else {
    feature_filters_[feature].reset();
    enabled_features_.reset(feature);
    if (enabled_features_.none()) {
      poll_timer_.Stop();
      ShutDown();
    }
    return true;
  }
}

bool DBusAdaptor::GetFeatureResult(brillo::ErrorPtr* error,
                                   HpsResultProto* result,
                                   uint8_t feature) {
  if (!enabled_features_.test(feature)) {
    brillo::Error::AddTo(error, FROM_HERE, brillo::errors::dbus::kDomain,
                         kErrorPath, "hpsd: Feature not enabled.");

    return false;
  }
  DCHECK(feature_filters_[feature]);
  result->set_value(feature_filters_[feature]->GetCurrentResult());
  return true;
}

bool DBusAdaptor::EnableHpsSense(brillo::ErrorPtr* error,
                                 const hps::FeatureConfig& config) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return EnableFeature(
      error, config, 0,
      base::BindRepeating(&DBusAdaptor::SendHpsSenseChangedSignal,
                          base::Unretained(this)));
}

bool DBusAdaptor::DisableHpsSense(brillo::ErrorPtr* error) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (DisableFeature(error, 0)) {
    DBusAdaptor::SendHpsSenseChangedSignal(
        HpsResultToSerializedBytes(HpsResult::UNKNOWN));
    return true;
  }
  return false;
}

bool DBusAdaptor::GetResultHpsSense(brillo::ErrorPtr* error,
                                    HpsResultProto* result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return GetFeatureResult(error, result, 0);
}

bool DBusAdaptor::EnableHpsNotify(brillo::ErrorPtr* error,
                                  const hps::FeatureConfig& config) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return EnableFeature(
      error, config, 1,
      base::BindRepeating(&DBusAdaptor::SendHpsNotifyChangedSignal,
                          base::Unretained(this)));
}

bool DBusAdaptor::DisableHpsNotify(brillo::ErrorPtr* error) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (DisableFeature(error, 1)) {
    DBusAdaptor::SendHpsNotifyChangedSignal(
        HpsResultToSerializedBytes(HpsResult::UNKNOWN));
    return true;
  }
  return false;
}

bool DBusAdaptor::GetResultHpsNotify(brillo::ErrorPtr* error,
                                     HpsResultProto* result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return GetFeatureResult(error, result, 1);
}

}  // namespace hps
