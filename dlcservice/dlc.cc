// Copyright 2020 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "dlcservice/dlc.h"

#include <algorithm>
#include <cinttypes>
#include <utility>
#include <vector>

#include <base/strings/stringprintf.h>
#include <base/strings/string_number_conversions.h>
#include <base/strings/string_util.h>
#include <brillo/errors/error.h>
#include <chromeos/dbus/service_constants.h>
#include <dbus/dlcservice/dbus-constants.h>

#include "dlcservice/error.h"
#include "dlcservice/prefs.h"
#include "dlcservice/system_state.h"
#include "dlcservice/utils.h"

using std::string;
using std::vector;

namespace dlcservice {
// TODO(ahassani): Instead of initlialize function, create a factory method so
// we can develop different types of DLC classes.
bool DlcBase::Initialize() {
  const auto* system_state = SystemState::Get();
  const auto& manifest_dir = system_state->manifest_dir();
  package_ = *ScanDirectory(manifest_dir.Append(id_)).begin();
  if (!GetDlcManifest(system_state->manifest_dir(), id_, package_,
                      &manifest_)) {
    // Failing to read the manifest will be considered a blocker.
    LOG(ERROR) << "Failed to read the manifest of DLC " << id_;
    return false;
  }

  const auto& content_dir = system_state->content_dir();
  content_id_path_ = content_dir.Append(id_);
  content_package_path_ = content_id_path_.Append(package_);
  prefs_path_ = system_state->dlc_prefs_dir().Append(id_);

  state_.set_state(
      Prefs(*this, system_state->active_boot_slot()).Exists(kDlcPrefVerified)
          ? DlcState::MOUNTABLE
          : DlcState::NOT_INSTALLED);

  return true;
}

DlcId DlcBase::GetId() const {
  return id_;
}

DlcState DlcBase::GetState() const {
  return state_;
}

bool DlcBase::IsInstalling() const {
  return state_.state() == DlcState::INSTALLING;
}

bool DlcBase::IsInstalled() const {
  return state_.state() == DlcState::INSTALLED;
}

bool DlcBase::IsMountable() const {
  return state_.state() == DlcState::MOUNTABLE;
}

bool DlcBase::IsPreloadAllowed() const {
  return manifest_.preload_allowed();
}

base::FilePath DlcBase::GetRoot() const {
  if (mount_point_.empty())
    return {};
  return JoinPaths(mount_point_, kRootDirectoryInsideDlcModule);
}

bool DlcBase::MarkMountable(const BootSlot::Slot& slot, ErrorPtr* err) const {
  if (!Prefs(*this, slot).Create(kDlcPrefVerified)) {
    *err = Error::Create(
        kErrorInternal,
        base::StringPrintf(
            "Failed to persist kDlcPrefVerified pref for DLC=%s, Slot=%s",
            id_.c_str(), BootSlot::ToString(slot).c_str()));
    return false;
  }
  return true;
}

bool DlcBase::ClearMountable(const BootSlot::Slot& slot, ErrorPtr* err) const {
  if (!Prefs(*this, slot).Delete(kDlcPrefVerified)) {
    *err = Error::Create(
        kErrorInternal,
        base::StringPrintf(
            "Failed to remove kDlcPrefVerified pref for DLC=%s, Slot=%s",
            id_.c_str(), BootSlot::ToString(slot).c_str()));
    return false;
  }
  return true;
}

FilePath DlcBase::GetImagePath(BootSlot::Slot slot) const {
  return JoinPaths(content_package_path_, BootSlot::ToString(slot),
                   kDlcImageFileName);
}

bool DlcBase::Create(ErrorPtr* err) {
  // Create content directories.
  for (const auto& path : {content_id_path_, content_package_path_}) {
    if (!CreateDir(path)) {
      *err = Error::Create(
          kErrorInternal,
          base::StringPrintf("Failed to create directory %s for DLC=%s",
                             path.value().c_str(), id_.c_str()));
      return false;
    }
  }

  const int64_t image_size = manifest_.preallocated_size();
  if (image_size <= 0) {
    *err = Error::Create(
        kErrorInternal, base::StringPrintf("Preallocated size=%" PRId64
                                           " in manifest is illegal for DLC=%s",
                                           image_size, id_.c_str()));
    return false;
  }

  // Creates image A and B.
  for (const auto& slot : {BootSlot::Slot::A, BootSlot::Slot::B}) {
    FilePath image_path = GetImagePath(slot);
    if (!CreateFile(image_path, image_size)) {
      *err = Error::Create(
          kErrorAllocation,
          base::StringPrintf("Failed to create image file %s for DLC=%s",
                             image_path.value().c_str(), id_.c_str()));
      return false;
    }
  }

  state_.set_state(DlcState::INSTALLING);
  return true;
}

bool DlcBase::ValidateInactiveImage() const {
  const FilePath& inactive_image_path =
      GetImagePath(SystemState::Get()->inactive_boot_slot());
  const int64_t& max_image_size = manifest_.preallocated_size();

  if (!base::PathExists(inactive_image_path)) {
    LOG(WARNING) << "The DLC image " << inactive_image_path.value()
                 << " does not exist.";
    if (!CreateFile(inactive_image_path, max_image_size)) {
      LOG(ERROR) << "Failed to create inactive image "
                 << inactive_image_path.value()
                 << " during validation for DLC=" << id_;
      return false;
    }
  }

  // Different scenarios possible to hit this flow:
  //  - Inactive and manifest size are the same -> Do nothing.
  //
  // TODO(crbug.com/943780): This requires further design updates to both
  //  dlcservice and upate_engine in order to fully handle. Solution pending.
  //  - Update applied and not rebooted -> Do nothing. A lot more corner cases
  //    than just always keeping active and inactive image sizes the same.
  //
  //  - Update applied and rebooted -> Try fixing up inactive image.
  int64_t inactive_image_size;
  if (!base::GetFileSize(inactive_image_path, &inactive_image_size)) {
    LOG(ERROR) << "Failed to get inactive image size DLC=" << id_;
  } else {
    // When |inactive_image_size| is less than the size permitted in the
    // manifest, this means that we rebooted into an update.
    if (inactive_image_size < max_image_size) {
      // Only increasing size, the inactive DLC is still usable in case of
      // reverts.
      if (!ResizeFile(inactive_image_path, max_image_size)) {
        LOG(ERROR) << "Failed to increase inactive image, update_engine may "
                      "face problems in updating when stateful is full later.";
        return false;
      }
    }
  }
  return true;
}

bool DlcBase::PreloadedCopier() const {
  FilePath image_preloaded_path =
      JoinPaths(SystemState::Get()->preloaded_content_dir(), id_, package_,
                kDlcImageFileName);
  int64_t max_image_size = manifest_.preallocated_size();
  // Scope the |image_preloaded| file so it always closes before deleting.
  {
    int64_t image_preloaded_size;
    if (!base::GetFileSize(image_preloaded_path, &image_preloaded_size)) {
      LOG(ERROR) << "Failed to get preloaded DLC (" << id_ << ") size.";
      return false;
    }
    if (image_preloaded_size > max_image_size) {
      LOG(ERROR) << "Preloaded DLC (" << id_ << ") is (" << image_preloaded_size
                 << ") larger than the preallocated size (" << max_image_size
                 << ") in manifest.";
      return false;
    }
  }

  // Based on the current boot slot, copy the preloadable image.
  FilePath image_boot_path, image_non_boot_path;
  if (SystemState::Get()->active_boot_slot() == BootSlot::Slot::A) {
    image_boot_path = GetImagePath(BootSlot::Slot::A);
    image_non_boot_path = GetImagePath(BootSlot::Slot::B);
  } else {
    image_boot_path = GetImagePath(BootSlot::Slot::B);
    image_non_boot_path = GetImagePath(BootSlot::Slot::A);
  }
  // TODO(kimjae): when preloaded images are place into unencrypted, this
  // operation can be a move.
  string sha256;
  if (!CopyAndHashFile(image_preloaded_path, image_boot_path, &sha256)) {
    LOG(ERROR) << "Failed to preload DLC (" << id_ << ") into boot slot path ("
               << image_boot_path << ")";
    return false;
  }

  auto manifest_sha256 = manifest_.image_sha256();
  if (sha256 !=
      base::HexEncode(manifest_sha256.data(), manifest_sha256.size())) {
    LOG(ERROR) << "Image is corrupted or modified for DLC=" << id_;
    return false;
  }

  if (!ResizeFile(image_boot_path, max_image_size)) {
    LOG(ERROR) << "Image failed to resize for DLC=" << id_
               << ", Path=" << image_boot_path << " ,Size=" << max_image_size;
    return false;
  }

  ErrorPtr tmp_err;
  if (!MarkMountable(SystemState::Get()->active_boot_slot(), &tmp_err)) {
    LOG(ERROR) << Error::ToString(tmp_err);
    return false;
  }

  return true;
}

void DlcBase::PreloadImage() {
  // Deleting DLC(s) that might already be installed as preloading DLC
  // take precedence in order to allow stale DLC in cache to be cleared.
  // Loading should be run prior to preloading, to enforce this strict
  // precedence.
  // TODO(crbug.com/1059445): Verify before deleting that image to preload
  // has the correct hash.
  ErrorPtr tmp_err;
  if (!DeleteInternal(&tmp_err)) {
    PLOG(ERROR) << "Failed to delete prior to preloading DLC=" << id_ << ", "
                << Error::ToString(tmp_err);
    return;
  }

  if (!InitInstall(&tmp_err)) {
    LOG(ERROR) << "Failed to create preloaded DLC=" << id_ << ", "
               << Error::ToString(tmp_err);
    return;
  }

  if (!PreloadedCopier()) {
    LOG(ERROR) << "Something went wrong during preloading DLC (" << id_
               << "), please check for previous errors.";
    if (!CancelInstall(&tmp_err))
      LOG(WARNING) << Error::ToString(tmp_err);
    return;
  }

  // When the copying is successful, go ahead and finish installation.
  if (!FinishInstall(&tmp_err)) {
    LOG(ERROR) << "Failed to finish installation for preloaded DLC=" << id_
               << ", " << Error::ToString(tmp_err);
    return;
  }

  // Delete the preloaded DLC only after both copies into A and B succeed as
  // well as mounting.
  const auto path = SystemState::Get()->preloaded_content_dir().Append(id_);
  if (!base::DeleteFile(path, true)) {
    LOG(ERROR) << "Failed to delete preloaded DLC=" << id_;
  }
}

bool DlcBase::InitInstall(ErrorPtr* err) {
  if (!base::PathExists(prefs_path_)) {
    if (!CreateDir(prefs_path_)) {
      *err = Error::Create(kErrorInternal, "Failed to create prefs directory.");
      return false;
    }
  }

  switch (state_.state()) {
    case DlcState::NOT_INSTALLED:
      if (IsActiveImagePresent()) {
        if (!DeleteInternal(err)) {
          if (!CancelInstall(err))
            LOG(ERROR) << "Failed during install initialization: "
                       << Error::ToString(*err);
          return false;
        }
      }
      if (!Create(err)) {
        if (!CancelInstall(err))
          LOG(ERROR) << "Failed during install initialization: "
                     << Error::ToString(*err);
        return false;
      }
      break;
    case DlcState::MOUNTABLE:
      if (!ValidateInactiveImage())
        LOG(ERROR) << "Bad inactive image for DLC=" << id_;
      if (!TryMount())
        LOG(ERROR) << "Mounting mountable image failed for DLC=" << id_;
      break;
    case DlcState::INSTALLED:
      if (!ValidateInactiveImage())
        LOG(ERROR) << "Bad inactive image for DLC=" << id_;
      break;
    case DlcState::INSTALLING:
    default:
      NOTREACHED();
      return false;
  }
  // Failure to set the metadata flags should not fail the install.
  if (!SystemState::Get()->update_engine()->SetDlcActiveValue(true, id_, err)) {
    LOG(WARNING) << "Update Engine failed to set DLC to active:" << id_
                 << (*err ? Error::ToString(*err)
                          : "Missing error from update engine proxy.");
  }
  return true;
}

bool DlcBase::FinishInstall(ErrorPtr* err) {
  switch (state_.state()) {
    case DlcState::NOT_INSTALLED:
    case DlcState::MOUNTABLE:
    case DlcState::INSTALLED:
      return true;
    case DlcState::INSTALLING: {
      bool ret = true;
      if (!Prefs(*this, SystemState::Get()->active_boot_slot())
               .Exists(kDlcPrefVerified)) {
        *err =
            Error::Create(kErrorInternal,
                          base::StringPrintf("Cannot mount image which is not "
                                             "marked as mountable for DLC=%s",
                                             id_.c_str()));
        LOG(ERROR) << "Failed during install finalization: "
                   << Error::ToString(*err);
        ret = false;
      } else if (!Mount(err)) {
        LOG(ERROR) << "Failed during install finalization: "
                   << Error::ToString(*err) << " for DLC=" << id_;
        ret = false;
      }
      if (!ret) {
        ErrorPtr tmp_err;
        if (!DeleteInternal(&tmp_err))
          LOG(ERROR) << "Failed during install finalization: "
                     << Error::ToString(tmp_err) << " for DLC=" << id_;
      }
      return ret;
    }
    default:
      NOTREACHED();
      return false;
  }
  return true;
}

bool DlcBase::CancelInstall(ErrorPtr* err) {
  if (!IsInstalling()) {
    return true;
  }
  // Consider as not installed even if delete fails below, correct errors
  // will be propagated later and should not block on further installs.
  if (!DeleteInternal(err)) {
    PLOG(ERROR) << "Failed during install cancellation: "
                << Error::ToString(*err) << " for DLC " << id_;
    return false;
  }
  return true;
}

bool DlcBase::Mount(ErrorPtr* err) {
  string mount_point;
  if (!SystemState::Get()->image_loader()->LoadDlcImage(
          id_, package_,
          SystemState::Get()->active_boot_slot() == BootSlot::Slot::A
              ? imageloader::kSlotNameA
              : imageloader::kSlotNameB,
          &mount_point, nullptr, kImageLoaderTimeoutMs)) {
    *err = Error::Create(kErrorInternal,
                         "Imageloader is unavailable for LoadDlcImage().");
    return false;
  }
  if (mount_point.empty()) {
    *err = Error::Create(kErrorInternal,
                         "Imageloader LoadDlcImage() call failed.");
    return false;
  }
  mount_point_ = FilePath(mount_point);
  state_.set_state(DlcState::INSTALLED);
  return true;
}

bool DlcBase::Unmount(ErrorPtr* err) {
  bool success = false;
  if (!SystemState::Get()->image_loader()->UnloadDlcImage(
          id_, package_, &success, nullptr, kImageLoaderTimeoutMs)) {
    *err = Error::Create(kErrorInternal,
                         "Imageloader is unavailable for UnloadDlcImage().");
    return false;
  }
  if (!success) {
    *err = Error::Create(kErrorInternal,
                         "Imageloader UnloadDlcImage() call failed.");
    return false;
  }
  state_.set_state(DlcState::NOT_INSTALLED);
  return true;
}

bool DlcBase::TryMount() {
  if (!mount_point_.empty() && base::PathExists(GetRoot())) {
    LOG(INFO) << "Skipping mount as already mounted at " << GetRoot();
    state_.set_state(DlcState::INSTALLED);
    return true;
  }

  ErrorPtr tmp_err;
  if (!Mount(&tmp_err)) {
    LOG(ERROR) << "DLC thought to have been installed, but maybe is in a "
               << "bad state. DLC=" << id_ << ", " << Error::ToString(tmp_err);
    return false;
  }
  return true;
}

bool DlcBase::IsActiveImagePresent() const {
  return base::PathExists(GetImagePath(SystemState::Get()->active_boot_slot()));
}

// Deletes all directories related to this DLC.
bool DlcBase::DeleteInternal(ErrorPtr* err) {
  vector<string> undeleted_paths;
  auto content_dir = SystemState::Get()->content_dir();
  for (const auto& path :
       {content_id_path_, content_package_path_, prefs_path_}) {
    if (!base::DeleteFile(path, true)) {
      PLOG(ERROR) << "Failed to delete path=" << path;
      undeleted_paths.push_back(path.value());
    }
  }
  // Failure to set DLC to inactive should not fail uninstall.
  ErrorPtr tmp_err;
  if (!SystemState::Get()->update_engine()->SetDlcActiveValue(false, id_,
                                                              &tmp_err))
    LOG(WARNING) << "Failed to set DLC(" << id_ << ") to inactive."
                 << (tmp_err ? Error::ToString(tmp_err)
                             : "Missing error from update engine proxy.");
  state_.set_state(DlcState::NOT_INSTALLED);
  if (!Prefs(*this, SystemState::Get()->active_boot_slot())
           .Delete(kDlcPrefVerified))
    LOG(ERROR) << "Failed to remove active kDlcPrefVerified pref for DLC="
               << id_;
  if (!Prefs(*this, SystemState::Get()->inactive_boot_slot())
           .Delete(kDlcPrefVerified))
    LOG(ERROR) << "Failed to remove inactive kDlcPrefVerified pref for DLC="
               << id_;
  if (!undeleted_paths.empty()) {
    *err = Error::Create(
        kErrorInternal,
        base::StringPrintf("DLC directories (%s) could not be deleted.",
                           base::JoinString(undeleted_paths, ",").c_str()));
    return false;
  }
  return true;
}

bool DlcBase::Delete(ErrorPtr* err) {
  switch (state_.state()) {
    case DlcState::NOT_INSTALLED:
      LOG(WARNING) << "Trying to uninstall not installed DLC=" << id_;
      return DeleteInternal(err);
    case DlcState::INSTALLING:
      *err = Error::Create(
          kErrorBusy,
          base::StringPrintf("Trying to delete a currently installing DLC=%s",
                             id_.c_str()));
      return false;
    case DlcState::MOUNTABLE:
      LOG(WARNING) << "Uninstalling mountable but not mounted DLC=" << id_;
      return DeleteInternal(err);
    case DlcState::INSTALLED:
      return Unmount(err) && DeleteInternal(err);
    default:
      NOTREACHED();
      return false;
  }
}

}  // namespace dlcservice
