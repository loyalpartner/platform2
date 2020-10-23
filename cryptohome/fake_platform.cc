// Copyright 2020 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Contains the implementation of class Platform

#include "cryptohome/fake_platform.h"

#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

#include <base/files/file_path.h>
#include <brillo/secure_blob.h>

namespace cryptohome {

// Constructor/destructor

FakePlatform::FakePlatform() : Platform() {
  base::GetTempDir(&tmpfs_rootfs_);
  tmpfs_rootfs_ = tmpfs_rootfs_.Append(real_platform_.GetRandomSuffix());
  if (!real_platform_.CreateDirectory(tmpfs_rootfs_)) {
    LOG(ERROR) << "Failed to create test dir: " << tmpfs_rootfs_;
  }
}

FakePlatform::~FakePlatform() {
  real_platform_.DeleteFile(tmpfs_rootfs_, true /* recursive */);
}

// Helpers

base::FilePath FakePlatform::TestFilePath(const base::FilePath& path) const {
  std::string path_str = path.NormalizePathSeparators().value();
  // Make the path relative.
  CHECK(path.IsAbsolute());
  if (path_str.length() > 0 && path_str[0] == '/') {
    path_str = path_str.substr(1);
  }
  return tmpfs_rootfs_.Append(path_str);
}

// Platform API

bool FakePlatform::Rename(const base::FilePath& from,
                          const base::FilePath& to) {
  return real_platform_.Rename(TestFilePath(from), TestFilePath(to));
}

bool FakePlatform::Move(const base::FilePath& from, const base::FilePath& to) {
  return real_platform_.Move(TestFilePath(from), TestFilePath(to));
}

bool FakePlatform::Copy(const base::FilePath& from, const base::FilePath& to) {
  return real_platform_.Copy(TestFilePath(from), TestFilePath(to));
}

bool FakePlatform::EnumerateDirectoryEntries(
    const base::FilePath& path,
    bool recursive,
    std::vector<base::FilePath>* ent_list) {
  return real_platform_.EnumerateDirectoryEntries(TestFilePath(path), recursive,
                                                  ent_list);
}

bool FakePlatform::DeleteFile(const base::FilePath& path, bool recursive) {
  return real_platform_.DeleteFile(TestFilePath(path), recursive);
}

bool FakePlatform::DeleteFileDurable(const base::FilePath& path,
                                     bool recursive) {
  return real_platform_.DeleteFileDurable(TestFilePath(path), recursive);
}

bool FakePlatform::FileExists(const base::FilePath& path) {
  return real_platform_.FileExists(TestFilePath(path));
}

bool FakePlatform::DirectoryExists(const base::FilePath& path) {
  return real_platform_.DirectoryExists(TestFilePath(path));
}

bool FakePlatform::CreateDirectory(const base::FilePath& path) {
  return real_platform_.CreateDirectory(TestFilePath(path));
}

bool FakePlatform::ReadFile(const base::FilePath& path, brillo::Blob* blob) {
  return real_platform_.ReadFile(TestFilePath(path), blob);
}

bool FakePlatform::ReadFileToString(const base::FilePath& path,
                                    std::string* str) {
  return real_platform_.ReadFileToString(TestFilePath(path), str);
}

bool FakePlatform::ReadFileToSecureBlob(const base::FilePath& path,
                                        brillo::SecureBlob* sblob) {
  return real_platform_.ReadFileToSecureBlob(TestFilePath(path), sblob);
}

bool FakePlatform::WriteFile(const base::FilePath& path,
                             const brillo::Blob& blob) {
  return real_platform_.WriteFile(TestFilePath(path), blob);
}

bool FakePlatform::WriteSecureBlobToFile(const base::FilePath& path,
                                         const brillo::SecureBlob& sblob) {
  return real_platform_.WriteSecureBlobToFile(TestFilePath(path), sblob);
}

bool FakePlatform::WriteFileAtomic(const base::FilePath& path,
                                   const brillo::Blob& blob,
                                   mode_t mode) {
  return real_platform_.WriteFileAtomic(TestFilePath(path), blob, mode);
}

bool FakePlatform::WriteSecureBlobToFileAtomic(const base::FilePath& path,
                                               const brillo::SecureBlob& sblob,
                                               mode_t mode) {
  return real_platform_.WriteSecureBlobToFileAtomic(TestFilePath(path), sblob,
                                                    mode);
}

bool FakePlatform::WriteFileAtomicDurable(const base::FilePath& path,
                                          const brillo::Blob& blob,
                                          mode_t mode) {
  return real_platform_.WriteFileAtomicDurable(TestFilePath(path), blob, mode);
}

bool FakePlatform::WriteSecureBlobToFileAtomicDurable(
    const base::FilePath& path, const brillo::SecureBlob& sblob, mode_t mode) {
  return real_platform_.WriteSecureBlobToFileAtomicDurable(TestFilePath(path),
                                                           sblob, mode);
}

bool FakePlatform::WriteStringToFile(const base::FilePath& path,
                                     const std::string& str) {
  return real_platform_.WriteStringToFile(TestFilePath(path), str);
}

bool FakePlatform::WriteStringToFileAtomicDurable(const base::FilePath& path,
                                                  const std::string& str,
                                                  mode_t mode) {
  return real_platform_.WriteStringToFileAtomicDurable(TestFilePath(path), str,
                                                       mode);
}

bool FakePlatform::WriteArrayToFile(const base::FilePath& path,
                                    const char* data,
                                    size_t size) {
  return real_platform_.WriteArrayToFile(TestFilePath(path), data, size);
}

FileEnumerator* FakePlatform::GetFileEnumerator(const base::FilePath& path,
                                                bool recursive,
                                                int file_type) {
  return real_platform_.GetFileEnumerator(TestFilePath(path), recursive,
                                          file_type);
}

bool FakePlatform::GetUserId(const std::string& user,
                             uid_t* user_id,
                             gid_t* group_id) const {
  CHECK(user_id);
  CHECK(group_id);

  if (uids_.find(user) == uids_.end() || gids_.find(user) == gids_.end()) {
    LOG(ERROR) << "No user: " << user;
    return false;
  }

  *user_id = uids_.at(user);
  *group_id = gids_.at(user);
  return true;
}

bool FakePlatform::GetGroupId(const std::string& group, gid_t* group_id) const {
  CHECK(group_id);

  if (gids_.find(group) == gids_.end()) {
    LOG(ERROR) << "No group: " << group;
    return false;
  }

  *group_id = gids_.at(group);
  return true;
}

// Test API

void FakePlatform::SetUserId(const std::string& user, uid_t user_id) {
  CHECK(uids_.find(user) == uids_.end());

  uids_[user] = user_id;
}

void FakePlatform::SetGroupId(const std::string& group, gid_t group_id) {
  CHECK(gids_.find(group) == gids_.end());

  gids_[group] = group_id;
}

void FakePlatform::SetStandardUsersAndGroups() {
  SetUserId(fake_platform::kRoot, fake_platform::kRootUID);
  SetGroupId(fake_platform::kRoot, fake_platform::kRootGID);
  SetUserId(fake_platform::kChapsUser, fake_platform::kChapsUID);
  SetGroupId(fake_platform::kChapsUser, fake_platform::kChapsGID);
  SetUserId(fake_platform::kChronosUser, fake_platform::kChronosUID);
  SetGroupId(fake_platform::kChronosUser, fake_platform::kChronosGID);
  SetGroupId(fake_platform::kSharedGroup, fake_platform::kSharedGID);
}

}  // namespace cryptohome
