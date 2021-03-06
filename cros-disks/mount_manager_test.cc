// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Unit tests for cros_disks::MountManager. See mount-manager.h for details
// on MountManager.

#include "cros-disks/mount_manager.h"

#include <sys/mount.h>
#include <sys/unistd.h>

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <base/callback.h>
#include <base/check.h>
#include <base/strings/strcat.h>
#include <brillo/process/process_reaper.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "cros-disks/metrics.h"
#include "cros-disks/mock_platform.h"
#include "cros-disks/mount_entry.h"
#include "cros-disks/mount_options.h"
#include "cros-disks/mount_point.h"
#include "cros-disks/mounter.h"
#include "cros-disks/platform.h"

using testing::_;
using testing::ByMove;
using testing::DoAll;
using testing::ElementsAre;
using testing::Invoke;
using testing::Return;
using testing::SetArgPointee;
using testing::StrictMock;
using testing::WithArgs;

namespace cros_disks {
namespace {

const char kMountRootDirectory[] = "/media/removable";
const char kSourcePath[] = "source";
const char kMountPath[] = "/media/removable/test";

}  // namespace

// A mock mount manager class for testing the mount manager base class.
class MountManagerUnderTest : public MountManager {
 public:
  MountManagerUnderTest(Platform* platform,
                        Metrics* metrics,
                        brillo::ProcessReaper* process_reaper)
      : MountManager(kMountRootDirectory, platform, metrics, process_reaper) {}

  ~MountManagerUnderTest() override { UnmountAll(); }

  MOCK_METHOD(bool, CanMount, (const std::string&), (const, override));
  MOCK_METHOD(MountSourceType, GetMountSourceType, (), (const, override));
  MOCK_METHOD(std::unique_ptr<MountPoint>,
              DoMount,
              (const std::string&,
               const std::string&,
               const std::vector<std::string>&,
               const base::FilePath&,
               MountErrorType*),
              (override));
  MOCK_METHOD(bool,
              ShouldReserveMountPathOnError,
              (MountErrorType),
              (const, override));
  MOCK_METHOD(std::string,
              SuggestMountPath,
              (const std::string&),
              (const, override));

  // Adds or updates a mapping |source_path| to its mount state in the cache.
  void AddMount(const std::string& source_path,
                std::unique_ptr<MountPoint> mount_point) {
    DCHECK(mount_point);
    DCHECK(!FindMountBySource(source_path));
    mount_states_.insert({source_path, std::move(mount_point)});
  }

  using MountManager::GetMountErrorOfReservedMountPath;
  using MountManager::ReserveMountPath;
  using MountManager::UnreserveMountPath;

  bool IsMountPathInCache(const std::string& path) {
    return FindMountByMountPath(base::FilePath(path));
  }

  bool IsMountPathReserved(const std::string& path) {
    return MountManager::IsMountPathReserved(base::FilePath(path));
  }

  void AddMountStateCache(const std::string& source,
                          std::unique_ptr<MountPoint> mount_point) {
    mount_states_.insert({source, std::move(mount_point)});
  }

  bool RemoveMountPathFromCache(const std::string& path) {
    MountPoint* mp = FindMountByMountPath(base::FilePath(path));
    if (!mp)
      return false;
    return RemoveMount(mp);
  }

  std::unordered_set<std::string> GetReservedMountPaths() {
    std::unordered_set<std::string> result;
    for (const auto& element : reserved_mount_paths_) {
      result.insert(element.first.value());
    }
    return result;
  }

  std::optional<MountEntry> GetMountEntryForTest(const std::string& source) {
    MountPoint* mp = FindMountBySource(source);
    if (!mp)
      return {};
    return MountEntry(MOUNT_ERROR_NONE, source, GetMountSourceType(),
                      mp->path().value(), mp->is_read_only());
  }
};

class MountManagerTest : public ::testing::Test {
 public:
  MountManagerTest() : manager_(&platform_, &metrics_, &process_reaper_) {
    EXPECT_CALL(manager_, GetMountSourceType())
        .WillRepeatedly(Return(MOUNT_SOURCE_REMOVABLE_DEVICE));
    EXPECT_CALL(platform_, GetRealPath(_, _)).WillRepeatedly(Return(false));
  }

  std::unique_ptr<MountPoint> MakeMountPoint(const std::string& mount_path) {
    return MountPoint::CreateLeaking(base::FilePath(mount_path));
  }

  void OnMountCompleted(const std::string& path, MountErrorType error) {
    EXPECT_FALSE(mount_completed_);
    mount_path_ = path;
    mount_error_ = error;
    mount_completed_ = true;
  }

  MountManager::MountCallback GetMountCallback() {
    mount_path_.clear();
    mount_error_ = MOUNT_ERROR_NONE;
    mount_completed_ = false;

    return base::BindOnce(&MountManagerTest::OnMountCompleted,
                          base::Unretained(this));
  }

 protected:
  Metrics metrics_;
  StrictMock<MockPlatform> platform_;
  brillo::ProcessReaper process_reaper_;
  StrictMock<MountManagerUnderTest> manager_;
  std::string filesystem_type_;
  std::string mount_path_;
  MountErrorType mount_error_;
  bool mount_completed_;
  std::vector<std::string> options_;
};

// Verifies that MountManager::Initialize() returns false when it fails to
// create the mount root directory.
TEST_F(MountManagerTest, InitializeFailedInCreateDirectory) {
  EXPECT_CALL(platform_, CreateDirectory(kMountRootDirectory))
      .WillOnce(Return(false));
  EXPECT_CALL(platform_, SetOwnership(kMountRootDirectory, getuid(), getgid()))
      .Times(0);
  EXPECT_CALL(platform_, SetPermissions(kMountRootDirectory, _)).Times(0);

  EXPECT_FALSE(manager_.Initialize());
}

// Verifies that MountManager::Initialize() returns false when it fails to
// set the ownership of the created mount root directory.
TEST_F(MountManagerTest, InitializeFailedInSetOwnership) {
  EXPECT_CALL(platform_, CreateDirectory(kMountRootDirectory))
      .WillOnce(Return(true));
  EXPECT_CALL(platform_, SetOwnership(kMountRootDirectory, getuid(), getgid()))
      .WillOnce(Return(false));
  EXPECT_CALL(platform_, SetPermissions(kMountRootDirectory, _)).Times(0);

  EXPECT_FALSE(manager_.Initialize());
}

// Verifies that MountManager::Initialize() returns false when it fails to
// set the permissions of the created mount root directory.
TEST_F(MountManagerTest, InitializeFailedInSetPermissions) {
  EXPECT_CALL(platform_, CreateDirectory(kMountRootDirectory))
      .WillOnce(Return(true));
  EXPECT_CALL(platform_, SetOwnership(kMountRootDirectory, getuid(), getgid()))
      .WillOnce(Return(true));
  EXPECT_CALL(platform_, SetPermissions(kMountRootDirectory, _))
      .WillOnce(Return(false));

  EXPECT_FALSE(manager_.Initialize());
}

// Verifies that MountManager::Initialize() returns true when it creates
// the mount root directory with the specified ownership and permissions.
TEST_F(MountManagerTest, InitializeSucceeded) {
  EXPECT_CALL(platform_, CreateDirectory(kMountRootDirectory))
      .WillOnce(Return(true));
  EXPECT_CALL(platform_, SetOwnership(kMountRootDirectory, getuid(), getgid()))
      .WillOnce(Return(true));
  EXPECT_CALL(platform_, SetPermissions(kMountRootDirectory, _))
      .WillOnce(Return(true));

  EXPECT_TRUE(manager_.Initialize());
}

// Verifies that MountManager::Mount() returns an error when it is invoked
// to mount an empty source path.
TEST_F(MountManagerTest, MountFailedWithEmptySourcePath) {
  EXPECT_CALL(manager_, SuggestMountPath(_)).Times(0);
  EXPECT_CALL(platform_, CreateOrReuseEmptyDirectory(_)).Times(0);
  EXPECT_CALL(platform_, CreateOrReuseEmptyDirectoryWithFallback(_, _, _))
      .Times(0);
  EXPECT_CALL(platform_, RemoveEmptyDirectory(_)).Times(0);
  EXPECT_CALL(manager_, DoMount(_, _, _, _, _)).Times(0);

  manager_.Mount("", filesystem_type_, options_, GetMountCallback());
  EXPECT_TRUE(mount_completed_);
  EXPECT_EQ(MOUNT_ERROR_INVALID_ARGUMENT, mount_error_);
}

// Verifies that MountManager::Mount() returns an error when it is invoked
// without a given mount path and the suggested mount path is invalid.
TEST_F(MountManagerTest, MountFailedWithInvalidSuggestedMountPath) {
  EXPECT_CALL(manager_, SuggestMountPath(_))
      .WillRepeatedly(Return("/media/removable/../test/doc"));
  EXPECT_CALL(platform_, CreateOrReuseEmptyDirectory(_)).Times(0);
  EXPECT_CALL(platform_, CreateOrReuseEmptyDirectoryWithFallback(_, _, _))
      .Times(0);
  EXPECT_CALL(platform_, RemoveEmptyDirectory(_)).Times(0);
  EXPECT_CALL(manager_, DoMount(_, _, _, _, _)).Times(0);

  manager_.Mount(kSourcePath, filesystem_type_, options_, GetMountCallback());
  EXPECT_TRUE(mount_completed_);
  EXPECT_EQ(MOUNT_ERROR_INVALID_PATH, mount_error_);

  options_.push_back("mountlabel=custom_label");
  manager_.Mount(kSourcePath, filesystem_type_, options_, GetMountCallback());
  EXPECT_TRUE(mount_completed_);
  EXPECT_EQ(MOUNT_ERROR_INVALID_PATH, mount_error_);
}

// Verifies that MountManager::Mount() returns an error when it is invoked
// with an mount label that yields an invalid mount path.
TEST_F(MountManagerTest, MountFailedWithInvalidMountLabel) {
  options_.push_back("mountlabel=../custom_label");

  EXPECT_CALL(manager_, SuggestMountPath(kSourcePath))
      .WillOnce(Return(kSourcePath));
  EXPECT_CALL(platform_, CreateOrReuseEmptyDirectory(_)).Times(0);
  EXPECT_CALL(platform_, CreateOrReuseEmptyDirectoryWithFallback(_, _, _))
      .Times(0);
  EXPECT_CALL(platform_, RemoveEmptyDirectory(_)).Times(0);
  EXPECT_CALL(manager_, DoMount(_, _, _, _, _)).Times(0);

  manager_.Mount(kSourcePath, filesystem_type_, options_, GetMountCallback());
  EXPECT_TRUE(mount_completed_);
  EXPECT_EQ(MOUNT_ERROR_INVALID_PATH, mount_error_);
}

// Verifies that MountManager::Mount() returns an error when it fails to
// create the specified mount directory.
TEST_F(MountManagerTest, MountFailedInCreateOrReuseEmptyDirectory) {
  EXPECT_CALL(manager_, SuggestMountPath(kSourcePath))
      .WillOnce(Return(kMountPath));
  EXPECT_CALL(platform_, CreateOrReuseEmptyDirectoryWithFallback(_, _, _))
      .WillOnce(Return(false));
  EXPECT_CALL(platform_, RemoveEmptyDirectory(_)).Times(0);
  EXPECT_CALL(manager_, DoMount(_, _, _, _, _)).Times(0);

  manager_.Mount(kSourcePath, filesystem_type_, options_, GetMountCallback());
  EXPECT_TRUE(mount_completed_);
  EXPECT_EQ(MOUNT_ERROR_DIRECTORY_CREATION_FAILED, mount_error_);
  EXPECT_EQ("", mount_path_);
}

// Verifies that MountManager::Mount() returns an error when it fails to
// create a mount directory after a number of trials.
TEST_F(MountManagerTest, MountFailedInCreateOrReuseEmptyDirectoryWithFallback) {
  EXPECT_CALL(manager_, SuggestMountPath(kSourcePath))
      .WillOnce(Return(kMountPath));
  EXPECT_CALL(platform_, CreateOrReuseEmptyDirectoryWithFallback(_, _, _))
      .WillOnce(Return(false));
  EXPECT_CALL(platform_, RemoveEmptyDirectory(_)).Times(0);
  EXPECT_CALL(manager_, DoMount(_, _, _, _, _)).Times(0);

  manager_.Mount(kSourcePath, filesystem_type_, options_, GetMountCallback());
  EXPECT_TRUE(mount_completed_);
  EXPECT_EQ(MOUNT_ERROR_DIRECTORY_CREATION_FAILED, mount_error_);
  EXPECT_EQ("", mount_path_);
  EXPECT_FALSE(manager_.IsMountPathInCache(kMountPath));
}

// Verifies that MountManager::Mount() fails when DoMount returns no MountPoint
// and no error (crbug.com/1317877 and crbug.com/1317878).
TEST_F(MountManagerTest, MountFailsWithNoMountPointAndNoError) {
  EXPECT_CALL(manager_, SuggestMountPath(kSourcePath))
      .WillOnce(Return(kMountPath));
  EXPECT_CALL(platform_, CreateOrReuseEmptyDirectoryWithFallback(_, _, _))
      .WillOnce(Return(true));

  EXPECT_CALL(manager_, DoMount(kSourcePath, filesystem_type_, options_,
                                base::FilePath(kMountPath), _))
      .WillOnce(
          DoAll(SetArgPointee<4>(MOUNT_ERROR_NONE), Return(ByMove(nullptr))));
  EXPECT_CALL(manager_, ShouldReserveMountPathOnError(MOUNT_ERROR_UNKNOWN))
      .WillOnce(Return(false));
  EXPECT_CALL(platform_, RemoveEmptyDirectory(kMountPath))
      .WillOnce(Return(true));

  manager_.Mount(kSourcePath, filesystem_type_, options_, GetMountCallback());
  EXPECT_TRUE(mount_completed_);
  EXPECT_EQ(MOUNT_ERROR_UNKNOWN, mount_error_);
  EXPECT_EQ("", mount_path_);
  EXPECT_FALSE(manager_.IsMountPathInCache(kMountPath));
  EXPECT_FALSE(manager_.IsMountPathReserved(kMountPath));
}

// Verifies that MountManager::Mount() fails when DoMount returns both a
// MountPoint and an error.
TEST_F(MountManagerTest, MountFailsWithMountPointAndError) {
  EXPECT_CALL(manager_, SuggestMountPath(kSourcePath))
      .WillOnce(Return(kMountPath));
  EXPECT_CALL(platform_, CreateOrReuseEmptyDirectoryWithFallback(_, _, _))
      .WillOnce(Return(true));

  const base::FilePath mount_path(kMountPath);
  auto ptr = std::make_unique<MountPoint>(
      MountPointData{
          .mount_path = mount_path,
          .source = kSourcePath,
          .flags = IsReadOnlyMount(options_) ? MS_RDONLY : 0,
      },
      &platform_);
  EXPECT_CALL(manager_,
              DoMount(kSourcePath, filesystem_type_, options_, mount_path, _))
      .WillOnce(DoAll(SetArgPointee<4>(MOUNT_ERROR_INVALID_PATH),
                      Return(ByMove(std::move(ptr)))));
  EXPECT_CALL(manager_, ShouldReserveMountPathOnError(MOUNT_ERROR_INVALID_PATH))
      .WillOnce(Return(false));
  EXPECT_CALL(platform_, Unmount(kMountPath, _))
      .WillOnce(Return(MOUNT_ERROR_NONE));
  EXPECT_CALL(platform_, RemoveEmptyDirectory(kMountPath))
      .WillOnce(Return(true))
      .WillOnce(Return(false));

  manager_.Mount(kSourcePath, filesystem_type_, options_, GetMountCallback());
  EXPECT_TRUE(mount_completed_);
  EXPECT_EQ(MOUNT_ERROR_INVALID_PATH, mount_error_);
  EXPECT_EQ("", mount_path_);
  EXPECT_FALSE(manager_.IsMountPathInCache(kMountPath));
  EXPECT_FALSE(manager_.IsMountPathReserved(kMountPath));
}

// Verifies that MountManager::Mount() returns no error when it successfully
// mounts a source path in read-write mode.
TEST_F(MountManagerTest, MountSucceededWithGivenMountPath) {
  options_.push_back("rw");

  EXPECT_CALL(manager_, SuggestMountPath(kSourcePath))
      .WillOnce(Return(kMountPath));
  EXPECT_CALL(platform_, CreateOrReuseEmptyDirectoryWithFallback(_, _, _))
      .WillOnce(Return(true));

  const base::FilePath mount_path(kMountPath);
  auto ptr = std::make_unique<MountPoint>(
      MountPointData{
          .mount_path = mount_path,
          .flags = IsReadOnlyMount(options_) ? MS_RDONLY : 0,
      },
      &platform_);
  EXPECT_CALL(manager_,
              DoMount(kSourcePath, filesystem_type_, options_, mount_path, _))
      .WillOnce(DoAll(SetArgPointee<4>(MOUNT_ERROR_NONE),
                      Return(ByMove(std::move(ptr)))));

  manager_.Mount(kSourcePath, filesystem_type_, options_, GetMountCallback());
  EXPECT_TRUE(mount_completed_);
  EXPECT_EQ(MOUNT_ERROR_NONE, mount_error_);
  EXPECT_EQ(kMountPath, mount_path_);
  EXPECT_TRUE(manager_.IsMountPathInCache(mount_path_));

  const std::optional<MountEntry> mount_entry =
      manager_.GetMountEntryForTest(kSourcePath);
  EXPECT_TRUE(mount_entry);
  EXPECT_FALSE(mount_entry->is_read_only);

  EXPECT_CALL(platform_, Unmount(mount_path_, _))
      .WillOnce(Return(MOUNT_ERROR_NONE));
  EXPECT_CALL(platform_, RemoveEmptyDirectory(kMountPath))
      .WillOnce(Return(true));
  EXPECT_TRUE(manager_.UnmountAll());
  EXPECT_FALSE(manager_.IsMountPathReserved(mount_path_));
}

// Verifies that MountManager::Mount() stores correct mount status in cache when
// read-only option is specified.
TEST_F(MountManagerTest, MountCachesStatusWithReadOnlyOption) {
  EXPECT_CALL(manager_, SuggestMountPath(kSourcePath))
      .WillOnce(Return(kMountPath));
  EXPECT_CALL(platform_, CreateOrReuseEmptyDirectoryWithFallback(_, _, _))
      .WillOnce(Return(true));

  // Add read-only mount option.
  options_.push_back("ro");

  base::FilePath mount_path(kMountPath);
  auto ptr = std::make_unique<MountPoint>(
      MountPointData{
          .mount_path = mount_path,
          .flags = IsReadOnlyMount(options_) ? MS_RDONLY : 0,
      },
      &platform_);
  EXPECT_CALL(manager_,
              DoMount(kSourcePath, filesystem_type_, options_, mount_path, _))
      .WillOnce(DoAll(SetArgPointee<4>(MOUNT_ERROR_NONE),
                      Return(ByMove(std::move(ptr)))));

  manager_.Mount(kSourcePath, filesystem_type_, options_, GetMountCallback());
  EXPECT_TRUE(mount_completed_);
  EXPECT_EQ(MOUNT_ERROR_NONE, mount_error_);
  EXPECT_EQ(kMountPath, mount_path_);
  EXPECT_TRUE(manager_.IsMountPathInCache(mount_path_));

  const std::optional<MountEntry> mount_entry =
      manager_.GetMountEntryForTest(kSourcePath);
  EXPECT_TRUE(mount_entry);
  EXPECT_TRUE(mount_entry->is_read_only);

  EXPECT_CALL(platform_, Unmount(mount_path_, _))
      .WillOnce(Return(MOUNT_ERROR_NONE));
  EXPECT_CALL(platform_, RemoveEmptyDirectory(mount_path_))
      .WillOnce(Return(true));
}

// Verifies that MountManager::Mount() stores correct mount status in cache when
// the mounter requested to mount in read-write mode but fell back to read-only
// mode.
TEST_F(MountManagerTest, MountSuccededWithReadOnlyFallback) {
  EXPECT_CALL(manager_, SuggestMountPath(kSourcePath))
      .WillOnce(Return(kMountPath));
  EXPECT_CALL(platform_, CreateOrReuseEmptyDirectoryWithFallback(_, _, _))
      .WillOnce(Return(true));
  options_.push_back("rw");
  // Emulate Mounter added read-only option as a fallback.
  const base::FilePath mount_path(kMountPath);
  auto ptr = std::make_unique<MountPoint>(
      MountPointData{
          .mount_path = mount_path,
          .flags = MS_RDONLY,
      },
      &platform_);
  EXPECT_CALL(manager_,
              DoMount(kSourcePath, filesystem_type_, options_, mount_path, _))
      .WillOnce(DoAll(SetArgPointee<4>(MOUNT_ERROR_NONE),
                      Return(ByMove(std::move(ptr)))));

  manager_.Mount(kSourcePath, filesystem_type_, options_, GetMountCallback());
  EXPECT_TRUE(mount_completed_);
  EXPECT_EQ(MOUNT_ERROR_NONE, mount_error_);
  EXPECT_EQ(kMountPath, mount_path_);
  EXPECT_TRUE(manager_.IsMountPathInCache(mount_path_));

  const std::optional<MountEntry> mount_entry =
      manager_.GetMountEntryForTest(kSourcePath);
  EXPECT_TRUE(mount_entry);
  EXPECT_TRUE(mount_entry->is_read_only);

  EXPECT_CALL(platform_, Unmount(mount_path_, _))
      .WillOnce(Return(MOUNT_ERROR_NONE));
  EXPECT_CALL(platform_, RemoveEmptyDirectory(mount_path_))
      .WillOnce(Return(true));
}

// Verifies that MountManager::Mount() returns no error when it successfully
// mounts a source path with no mount path specified.
TEST_F(MountManagerTest, MountSucceededWithEmptyMountPath) {
  EXPECT_CALL(manager_, SuggestMountPath(kSourcePath))
      .WillOnce(Return(kMountPath));
  EXPECT_CALL(platform_, CreateOrReuseEmptyDirectoryWithFallback(_, _, _))
      .WillOnce(Return(true));

  const base::FilePath mount_path(kMountPath);
  auto ptr = std::make_unique<MountPoint>(
      MountPointData{
          .mount_path = mount_path,
          .flags = IsReadOnlyMount(options_) ? MS_RDONLY : 0,
      },
      &platform_);
  EXPECT_CALL(manager_,
              DoMount(kSourcePath, filesystem_type_, options_, mount_path, _))
      .WillOnce(DoAll(SetArgPointee<4>(MOUNT_ERROR_NONE),
                      Return(ByMove(std::move(ptr)))));

  manager_.Mount(kSourcePath, filesystem_type_, options_, GetMountCallback());
  EXPECT_TRUE(mount_completed_);
  EXPECT_EQ(MOUNT_ERROR_NONE, mount_error_);
  EXPECT_EQ(kMountPath, mount_path_);
  EXPECT_TRUE(manager_.IsMountPathInCache(mount_path_));

  EXPECT_CALL(platform_, Unmount(mount_path_, _))
      .WillOnce(Return(MOUNT_ERROR_NONE));
  EXPECT_CALL(platform_, RemoveEmptyDirectory(mount_path_))
      .WillOnce(Return(true));
  EXPECT_TRUE(manager_.UnmountAll());
  EXPECT_FALSE(manager_.IsMountPathReserved(mount_path_));
}

// Verifies that MountManager::Mount() returns no error when it successfully
// mounts a source path with a given mount label in options.
TEST_F(MountManagerTest, MountSucceededWithGivenMountLabel) {
  const std::string final_mount_path =
      base::StrCat({kMountRootDirectory, "/custom_label"});
  options_.push_back("mountlabel=custom_label");
  std::vector<std::string> updated_options;

  EXPECT_CALL(manager_, SuggestMountPath(kSourcePath))
      .WillOnce(Return(kMountPath));
  EXPECT_CALL(platform_, CreateOrReuseEmptyDirectoryWithFallback(_, _, _))
      .WillOnce(Return(true));

  const base::FilePath mount_path(final_mount_path);
  auto ptr = std::make_unique<MountPoint>(
      MountPointData{
          .mount_path = mount_path,
          .flags = IsReadOnlyMount(options_) ? MS_RDONLY : 0,
      },
      &platform_);
  EXPECT_CALL(manager_,
              DoMount(kSourcePath, filesystem_type_, _, mount_path, _))
      .WillOnce(DoAll(SetArgPointee<4>(MOUNT_ERROR_NONE),
                      Return(ByMove(std::move(ptr)))));

  manager_.Mount(kSourcePath, filesystem_type_, options_, GetMountCallback());
  EXPECT_TRUE(mount_completed_);
  EXPECT_EQ(MOUNT_ERROR_NONE, mount_error_);
  EXPECT_EQ(final_mount_path, mount_path_);
  EXPECT_TRUE(manager_.IsMountPathInCache(mount_path_));

  EXPECT_CALL(platform_, Unmount(final_mount_path, _))
      .WillOnce(Return(MOUNT_ERROR_NONE));
  EXPECT_CALL(platform_, RemoveEmptyDirectory(final_mount_path))
      .WillOnce(Return(true));
  EXPECT_TRUE(manager_.UnmountAll());
  EXPECT_FALSE(manager_.IsMountPathReserved(mount_path_));
}

// Verifies that MountManager::Mount() handles the mounting of an already
// mounted source path properly.
TEST_F(MountManagerTest, MountWithAlreadyMountedSourcePath) {
  EXPECT_CALL(manager_, SuggestMountPath(kSourcePath))
      .WillOnce(Return(kMountPath));
  EXPECT_CALL(platform_, CreateOrReuseEmptyDirectoryWithFallback(_, _, _))
      .WillOnce(Return(true));

  const base::FilePath mount_path(kMountPath);
  auto ptr = std::make_unique<MountPoint>(
      MountPointData{
          .mount_path = mount_path,
          .flags = IsReadOnlyMount(options_) ? MS_RDONLY : 0,
      },
      &platform_);
  EXPECT_CALL(manager_,
              DoMount(kSourcePath, filesystem_type_, options_, mount_path, _))
      .WillOnce(DoAll(SetArgPointee<4>(MOUNT_ERROR_NONE),
                      Return(ByMove(std::move(ptr)))));

  manager_.Mount(kSourcePath, filesystem_type_, options_, GetMountCallback());
  EXPECT_TRUE(mount_completed_);
  EXPECT_EQ(MOUNT_ERROR_NONE, mount_error_);
  EXPECT_EQ(kMountPath, mount_path_);
  EXPECT_TRUE(manager_.IsMountPathInCache(mount_path_));

  // Mount an already-mounted source path
  manager_.Mount(kSourcePath, filesystem_type_, options_, GetMountCallback());
  EXPECT_TRUE(mount_completed_);
  EXPECT_EQ(MOUNT_ERROR_NONE, mount_error_);
  EXPECT_EQ(kMountPath, mount_path_);
  EXPECT_TRUE(manager_.IsMountPathInCache(mount_path_));

  // Mount an already-mounted source path
  manager_.Mount(kSourcePath, filesystem_type_, options_, GetMountCallback());
  EXPECT_TRUE(mount_completed_);
  EXPECT_EQ(MOUNT_ERROR_NONE, mount_error_);
  EXPECT_EQ(kMountPath, mount_path_);
  EXPECT_TRUE(manager_.IsMountPathInCache(mount_path_));

  // Unmount
  EXPECT_CALL(platform_, Unmount(kMountPath, _))
      .WillOnce(Return(MOUNT_ERROR_NONE));
  EXPECT_CALL(platform_, RemoveEmptyDirectory(kMountPath))
      .WillOnce(Return(true));
  EXPECT_TRUE(manager_.UnmountAll());
  EXPECT_FALSE(manager_.IsMountPathReserved(kMountPath));
}

// Verifies that MountManager::Mount() successfully reserves a path for a given
// type of error. A specific mount path is given in this case.
TEST_F(MountManagerTest, MountSucceededWithGivenMountPathInReservedCase) {
  EXPECT_CALL(manager_, SuggestMountPath(kSourcePath))
      .WillOnce(Return(kMountPath));
  EXPECT_CALL(platform_, CreateOrReuseEmptyDirectoryWithFallback(_, _, _))
      .WillOnce(Return(true));

  EXPECT_CALL(manager_, DoMount(kSourcePath, filesystem_type_, options_,
                                base::FilePath(kMountPath), _))
      .WillOnce(DoAll(SetArgPointee<4>(MOUNT_ERROR_UNKNOWN_FILESYSTEM),
                      Return(ByMove(nullptr))));
  EXPECT_CALL(manager_,
              ShouldReserveMountPathOnError(MOUNT_ERROR_UNKNOWN_FILESYSTEM))
      .WillOnce(Return(true));

  manager_.Mount(kSourcePath, filesystem_type_, options_, GetMountCallback());
  EXPECT_TRUE(mount_completed_);
  EXPECT_EQ(MOUNT_ERROR_UNKNOWN_FILESYSTEM, mount_error_);
  EXPECT_EQ(kMountPath, mount_path_);
  EXPECT_TRUE(manager_.IsMountPathInCache(mount_path_));
  EXPECT_TRUE(manager_.IsMountPathReserved(mount_path_));

  EXPECT_CALL(platform_, RemoveEmptyDirectory(kMountPath))
      .WillOnce(Return(true));
  EXPECT_TRUE(manager_.UnmountAll());
  EXPECT_FALSE(manager_.IsMountPathReserved(mount_path_));
  EXPECT_FALSE(manager_.IsMountPathReserved(mount_path_));
}

// Verifies that MountManager::Mount() successfully reserves a path for a given
// type of error. No specific mount path is given in this case.
TEST_F(MountManagerTest, MountSucceededWithEmptyMountPathInReservedCase) {
  EXPECT_CALL(manager_, SuggestMountPath(kSourcePath))
      .WillOnce(Return(kMountPath));
  EXPECT_CALL(platform_, CreateOrReuseEmptyDirectory(_)).Times(0);
  EXPECT_CALL(platform_, CreateOrReuseEmptyDirectoryWithFallback(_, _, _))
      .WillOnce(Return(true));
  EXPECT_CALL(platform_, RemoveEmptyDirectory(kMountPath))
      .WillOnce(Return(true));

  EXPECT_CALL(manager_, DoMount(kSourcePath, filesystem_type_, options_,
                                base::FilePath(kMountPath), _))
      .WillOnce(DoAll(SetArgPointee<4>(MOUNT_ERROR_UNKNOWN_FILESYSTEM),
                      Return(ByMove(nullptr))));
  EXPECT_CALL(manager_,
              ShouldReserveMountPathOnError(MOUNT_ERROR_UNKNOWN_FILESYSTEM))
      .WillOnce(Return(true));

  manager_.Mount(kSourcePath, filesystem_type_, options_, GetMountCallback());
  EXPECT_TRUE(mount_completed_);
  EXPECT_EQ(MOUNT_ERROR_UNKNOWN_FILESYSTEM, mount_error_);
  EXPECT_EQ(kMountPath, mount_path_);
  EXPECT_TRUE(manager_.IsMountPathInCache(mount_path_));
  EXPECT_TRUE(manager_.IsMountPathReserved(mount_path_));
  EXPECT_TRUE(manager_.UnmountAll());
  EXPECT_FALSE(manager_.IsMountPathInCache(mount_path_));
  EXPECT_FALSE(manager_.IsMountPathReserved(mount_path_));
}

// Verifies that MountManager::Mount() successfully reserves a path for a given
// type of error and returns the same error when it tries to mount the same path
// again.
TEST_F(MountManagerTest, MountSucceededWithAlreadyReservedMountPath) {
  EXPECT_CALL(platform_, CreateOrReuseEmptyDirectory(_)).Times(0);
  EXPECT_CALL(platform_, CreateOrReuseEmptyDirectoryWithFallback(_, _, _))
      .WillOnce(Return(true));
  EXPECT_CALL(platform_, RemoveEmptyDirectory(kMountPath))
      .WillOnce(Return(true));
  EXPECT_CALL(manager_, DoMount(kSourcePath, filesystem_type_, options_,
                                base::FilePath(kMountPath), _))
      .WillOnce(DoAll(SetArgPointee<4>(MOUNT_ERROR_UNKNOWN_FILESYSTEM),
                      Return(ByMove(nullptr))));
  EXPECT_CALL(manager_,
              ShouldReserveMountPathOnError(MOUNT_ERROR_UNKNOWN_FILESYSTEM))
      .WillOnce(Return(true));
  EXPECT_CALL(manager_, SuggestMountPath(kSourcePath))
      .WillOnce(Return(kMountPath));

  manager_.Mount(kSourcePath, filesystem_type_, options_, GetMountCallback());
  EXPECT_TRUE(mount_completed_);
  EXPECT_EQ(MOUNT_ERROR_UNKNOWN_FILESYSTEM, mount_error_);
  EXPECT_EQ(kMountPath, mount_path_);
  EXPECT_TRUE(manager_.IsMountPathInCache(mount_path_));
  EXPECT_TRUE(manager_.IsMountPathReserved(mount_path_));

  manager_.Mount(kSourcePath, filesystem_type_, options_, GetMountCallback());
  EXPECT_TRUE(mount_completed_);
  EXPECT_EQ(MOUNT_ERROR_UNKNOWN_FILESYSTEM, mount_error_);
  EXPECT_EQ(kMountPath, mount_path_);
  EXPECT_TRUE(manager_.IsMountPathInCache(mount_path_));
  EXPECT_TRUE(manager_.IsMountPathReserved(mount_path_));

  EXPECT_TRUE(manager_.UnmountAll());
  EXPECT_FALSE(manager_.IsMountPathInCache(mount_path_));
  EXPECT_FALSE(manager_.IsMountPathReserved(mount_path_));
}

// Verifies that MountManager::Mount() successfully reserves a path for a given
// type of error and returns the same error when it tries to mount the same path
// again.
TEST_F(MountManagerTest, MountFailedWithGivenMountPathInReservedCase) {
  EXPECT_CALL(manager_, SuggestMountPath(kSourcePath))
      .WillOnce(Return(kMountPath));
  EXPECT_CALL(platform_, CreateOrReuseEmptyDirectoryWithFallback(_, _, _))
      .WillOnce(Return(true));
  EXPECT_CALL(platform_, RemoveEmptyDirectory(kMountPath))
      .WillOnce(Return(true));
  EXPECT_CALL(manager_, DoMount(kSourcePath, filesystem_type_, options_,
                                base::FilePath(kMountPath), _))
      .WillOnce(DoAll(SetArgPointee<4>(MOUNT_ERROR_UNKNOWN_FILESYSTEM),
                      Return(ByMove(nullptr))));
  EXPECT_CALL(manager_,
              ShouldReserveMountPathOnError(MOUNT_ERROR_UNKNOWN_FILESYSTEM))
      .WillOnce(Return(true));

  manager_.Mount(kSourcePath, filesystem_type_, options_, GetMountCallback());
  EXPECT_TRUE(mount_completed_);
  EXPECT_EQ(MOUNT_ERROR_UNKNOWN_FILESYSTEM, mount_error_);
  EXPECT_EQ(kMountPath, mount_path_);
  EXPECT_TRUE(manager_.IsMountPathInCache(mount_path_));
  EXPECT_TRUE(manager_.IsMountPathReserved(mount_path_));
}

// Verifies that MountManager::Mount() fails to mount or reserve a path for
// a type of error that is not enabled for reservation.
TEST_F(MountManagerTest, MountFailedWithEmptyMountPathInReservedCase) {
  EXPECT_CALL(platform_, CreateOrReuseEmptyDirectory(_)).Times(0);
  EXPECT_CALL(platform_, CreateOrReuseEmptyDirectoryWithFallback(_, _, _))
      .WillOnce(Return(true));
  EXPECT_CALL(platform_, RemoveEmptyDirectory(kMountPath))
      .WillOnce(Return(true));
  EXPECT_CALL(manager_, DoMount(kSourcePath, filesystem_type_, options_,
                                base::FilePath(kMountPath), _))
      .WillOnce(DoAll(SetArgPointee<4>(MOUNT_ERROR_UNKNOWN_FILESYSTEM),
                      Return(ByMove(nullptr))));
  EXPECT_CALL(manager_,
              ShouldReserveMountPathOnError(MOUNT_ERROR_UNKNOWN_FILESYSTEM))
      .WillOnce(Return(false));
  EXPECT_CALL(manager_, SuggestMountPath(kSourcePath))
      .WillOnce(Return(kMountPath));

  manager_.Mount(kSourcePath, filesystem_type_, options_, GetMountCallback());
  EXPECT_TRUE(mount_completed_);
  EXPECT_EQ(MOUNT_ERROR_UNKNOWN_FILESYSTEM, mount_error_);
  EXPECT_EQ("", mount_path_);
  EXPECT_FALSE(manager_.IsMountPathInCache(mount_path_));
  EXPECT_FALSE(manager_.IsMountPathReserved(mount_path_));
}

// Verifies that MountManager::Unmount() returns an error when it is invoked
// to unmount an empty path.
TEST_F(MountManagerTest, UnmountFailedWithEmptyPath) {
  EXPECT_CALL(platform_, CreateOrReuseEmptyDirectory(_)).Times(0);
  EXPECT_CALL(platform_, CreateOrReuseEmptyDirectoryWithFallback(_, _, _))
      .Times(0);
  EXPECT_CALL(platform_, RemoveEmptyDirectory(_)).Times(0);
  EXPECT_CALL(manager_, DoMount(_, _, _, _, _)).Times(0);
  EXPECT_CALL(manager_, SuggestMountPath(_)).Times(0);

  EXPECT_EQ(MOUNT_ERROR_PATH_NOT_MOUNTED, manager_.Unmount(mount_path_));
}

// Verifies that MountManager::Unmount() returns an error when it fails to
// unmount a path that is not mounted.
TEST_F(MountManagerTest, UnmountFailedWithPathNotMounted) {
  mount_path_ = "nonexistent-path";

  EXPECT_CALL(platform_, CreateOrReuseEmptyDirectory(_)).Times(0);
  EXPECT_CALL(platform_, CreateOrReuseEmptyDirectoryWithFallback(_, _, _))
      .Times(0);
  EXPECT_CALL(platform_, RemoveEmptyDirectory(_)).Times(0);
  EXPECT_CALL(manager_, DoMount(_, _, _, _, _)).Times(0);
  EXPECT_CALL(manager_, SuggestMountPath(_)).Times(0);

  EXPECT_EQ(MOUNT_ERROR_PATH_NOT_MOUNTED, manager_.Unmount(mount_path_));
}

// Verifies that MountManager::Unmount() returns no error when it successfully
// unmounts a source path.
TEST_F(MountManagerTest, UnmountSucceededWithGivenSourcePath) {
  EXPECT_CALL(manager_, SuggestMountPath(kSourcePath))
      .WillOnce(Return(kMountPath));
  EXPECT_CALL(platform_, CreateOrReuseEmptyDirectoryWithFallback(_, _, _))
      .WillOnce(Return(true));

  const base::FilePath mount_path(kMountPath);
  auto ptr = std::make_unique<MountPoint>(
      MountPointData{
          .mount_path = mount_path,
          .flags = IsReadOnlyMount(options_) ? MS_RDONLY : 0,
      },
      &platform_);

  EXPECT_CALL(manager_,
              DoMount(kSourcePath, filesystem_type_, options_, mount_path, _))
      .WillOnce(DoAll(SetArgPointee<4>(MOUNT_ERROR_NONE),
                      Return(ByMove(std::move(ptr)))));

  manager_.Mount(kSourcePath, filesystem_type_, options_, GetMountCallback());
  EXPECT_TRUE(mount_completed_);
  EXPECT_EQ(MOUNT_ERROR_NONE, mount_error_);
  EXPECT_EQ(kMountPath, mount_path_);
  EXPECT_TRUE(manager_.IsMountPathInCache(mount_path_));

  EXPECT_CALL(platform_, Unmount(mount_path_, _))
      .WillOnce(Return(MOUNT_ERROR_NONE));
  EXPECT_CALL(platform_, RemoveEmptyDirectory(mount_path_))
      .WillOnce(Return(true));
  EXPECT_EQ(MOUNT_ERROR_NONE, manager_.Unmount(kSourcePath));
  EXPECT_FALSE(manager_.IsMountPathInCache(mount_path_));
}

// Verifies that MountManager::Unmount() returns no error when it successfully
// unmounts a mount path.
TEST_F(MountManagerTest, UnmountSucceededWithGivenMountPath) {
  EXPECT_CALL(manager_, SuggestMountPath(kSourcePath))
      .WillOnce(Return(kMountPath));
  EXPECT_CALL(platform_, CreateOrReuseEmptyDirectoryWithFallback(_, _, _))
      .WillOnce(Return(true));

  const base::FilePath mount_path(kMountPath);
  auto ptr = std::make_unique<MountPoint>(
      MountPointData{
          .mount_path = mount_path,
          .flags = IsReadOnlyMount(options_) ? MS_RDONLY : 0,
      },
      &platform_);

  EXPECT_CALL(manager_,
              DoMount(kSourcePath, filesystem_type_, options_, mount_path, _))
      .WillOnce(DoAll(SetArgPointee<4>(MOUNT_ERROR_NONE),
                      Return(ByMove(std::move(ptr)))));

  manager_.Mount(kSourcePath, filesystem_type_, options_, GetMountCallback());
  EXPECT_TRUE(mount_completed_);
  EXPECT_EQ(MOUNT_ERROR_NONE, mount_error_);
  EXPECT_EQ(kMountPath, mount_path_);
  EXPECT_TRUE(manager_.IsMountPathInCache(mount_path_));

  EXPECT_CALL(platform_, Unmount(mount_path_, _))
      .WillOnce(Return(MOUNT_ERROR_NONE));
  EXPECT_CALL(platform_, RemoveEmptyDirectory(mount_path_))
      .WillOnce(Return(true));
  EXPECT_EQ(MOUNT_ERROR_NONE, manager_.Unmount(mount_path_));
  EXPECT_FALSE(manager_.IsMountPathInCache(mount_path_));
}

// Verifies that MountManager::Unmount() removes mount path from cache if
// it appears to be not mounted.
TEST_F(MountManagerTest, UnmountRemovesFromCacheIfNotMounted) {
  EXPECT_CALL(manager_, SuggestMountPath(kSourcePath))
      .WillOnce(Return(kMountPath));
  EXPECT_CALL(platform_, CreateOrReuseEmptyDirectoryWithFallback(_, _, _))
      .WillOnce(Return(true));

  const base::FilePath mount_path(kMountPath);
  auto ptr = std::make_unique<MountPoint>(
      MountPointData{
          .mount_path = mount_path,
          .flags = IsReadOnlyMount(options_) ? MS_RDONLY : 0,
      },
      &platform_);

  EXPECT_CALL(manager_,
              DoMount(kSourcePath, filesystem_type_, options_, mount_path, _))
      .WillOnce(DoAll(SetArgPointee<4>(MOUNT_ERROR_NONE),
                      Return(ByMove(std::move(ptr)))));

  manager_.Mount(kSourcePath, filesystem_type_, options_, GetMountCallback());
  EXPECT_TRUE(mount_completed_);
  EXPECT_EQ(MOUNT_ERROR_NONE, mount_error_);
  EXPECT_EQ(kMountPath, mount_path_);
  EXPECT_TRUE(manager_.IsMountPathInCache(mount_path_));

  EXPECT_CALL(platform_, Unmount(mount_path_, _))
      .WillOnce(Return(MOUNT_ERROR_PATH_NOT_MOUNTED));
  EXPECT_CALL(platform_, RemoveEmptyDirectory(mount_path_))
      .WillOnce(Return(true));

  EXPECT_EQ(MOUNT_ERROR_PATH_NOT_MOUNTED, manager_.Unmount(mount_path_));
  EXPECT_FALSE(manager_.IsMountPathInCache(mount_path_));
}

// Verifies that MountManager::Unmount() returns no error when it is invoked
// to unmount the source path of a reserved mount path.
TEST_F(MountManagerTest, UnmountSucceededWithGivenSourcePathInReservedCase) {
  EXPECT_CALL(manager_, SuggestMountPath(kSourcePath))
      .WillOnce(Return(kMountPath));
  EXPECT_CALL(platform_, CreateOrReuseEmptyDirectoryWithFallback(_, _, _))
      .WillOnce(Return(true));

  const base::FilePath mount_path(kMountPath);
  EXPECT_CALL(manager_,
              DoMount(kSourcePath, filesystem_type_, options_, mount_path, _))
      .WillOnce(DoAll(SetArgPointee<4>(MOUNT_ERROR_UNKNOWN_FILESYSTEM),
                      Return(ByMove(nullptr))));
  EXPECT_CALL(manager_,
              ShouldReserveMountPathOnError(MOUNT_ERROR_UNKNOWN_FILESYSTEM))
      .WillOnce(Return(true));

  manager_.Mount(kSourcePath, filesystem_type_, options_, GetMountCallback());
  EXPECT_TRUE(mount_completed_);
  EXPECT_EQ(MOUNT_ERROR_UNKNOWN_FILESYSTEM, mount_error_);
  EXPECT_EQ(kMountPath, mount_path_);
  EXPECT_TRUE(manager_.IsMountPathInCache(mount_path_));
  EXPECT_TRUE(manager_.IsMountPathReserved(mount_path_));

  EXPECT_CALL(platform_, Unmount).Times(0);
  EXPECT_CALL(platform_, RemoveEmptyDirectory(mount_path_))
      .WillOnce(Return(true));
  EXPECT_EQ(MOUNT_ERROR_NONE, manager_.Unmount(kSourcePath));
  EXPECT_FALSE(manager_.IsMountPathInCache(mount_path_));
  EXPECT_FALSE(manager_.IsMountPathReserved(mount_path_));
}

// Verifies that MountManager::Unmount() returns no error when it is invoked
// to unmount a reserved mount path.
TEST_F(MountManagerTest, UnmountSucceededWithGivenMountPathInReservedCase) {
  EXPECT_CALL(manager_, SuggestMountPath(kSourcePath))
      .WillOnce(Return(kMountPath));
  EXPECT_CALL(platform_, CreateOrReuseEmptyDirectoryWithFallback(_, _, _))
      .WillOnce(Return(true));

  const base::FilePath mount_path(kMountPath);
  EXPECT_CALL(manager_,
              DoMount(kSourcePath, filesystem_type_, options_, mount_path, _))
      .WillOnce(DoAll(SetArgPointee<4>(MOUNT_ERROR_UNKNOWN_FILESYSTEM),
                      Return(ByMove(nullptr))));
  EXPECT_CALL(manager_,
              ShouldReserveMountPathOnError(MOUNT_ERROR_UNKNOWN_FILESYSTEM))
      .WillOnce(Return(true));

  manager_.Mount(kSourcePath, filesystem_type_, options_, GetMountCallback());
  EXPECT_TRUE(mount_completed_);
  EXPECT_EQ(MOUNT_ERROR_UNKNOWN_FILESYSTEM, mount_error_);
  EXPECT_EQ(kMountPath, mount_path_);
  EXPECT_TRUE(manager_.IsMountPathInCache(mount_path_));
  EXPECT_TRUE(manager_.IsMountPathReserved(mount_path_));

  EXPECT_CALL(platform_, Unmount).Times(0);
  EXPECT_CALL(platform_, RemoveEmptyDirectory(mount_path_))
      .WillOnce(Return(true));
  EXPECT_EQ(MOUNT_ERROR_NONE, manager_.Unmount(mount_path_));
  EXPECT_FALSE(manager_.IsMountPathInCache(mount_path_));
  EXPECT_FALSE(manager_.IsMountPathReserved(mount_path_));
}

// Verifies that MountManager::IsMountPathInCache() works as expected.
TEST_F(MountManagerTest, IsMountPathInCache) {
  mount_path_ = kMountPath;

  EXPECT_FALSE(manager_.IsMountPathInCache(mount_path_));
  manager_.AddMountStateCache(kSourcePath, MakeMountPoint(mount_path_));
  EXPECT_TRUE(manager_.IsMountPathInCache(mount_path_));
  EXPECT_TRUE(manager_.RemoveMountPathFromCache(mount_path_));
  EXPECT_FALSE(manager_.IsMountPathInCache(mount_path_));
}

// Verifies that MountManager::RemoveMountPathFromCache() works as expected.
TEST_F(MountManagerTest, RemoveMountPathFromCache) {
  mount_path_ = kMountPath;

  EXPECT_FALSE(manager_.RemoveMountPathFromCache(mount_path_));
  manager_.AddMountStateCache(kSourcePath, MakeMountPoint(mount_path_));
  EXPECT_TRUE(manager_.RemoveMountPathFromCache(mount_path_));
  EXPECT_FALSE(manager_.RemoveMountPathFromCache(mount_path_));
}

// Verifies that MountManager::GetReservedMountPaths() works as expected.
TEST_F(MountManagerTest, GetReservedMountPaths) {
  std::unordered_set<std::string> reserved_paths;
  std::unordered_set<std::string> expected_paths;
  base::FilePath path1("path1");
  base::FilePath path2("path2");

  reserved_paths = manager_.GetReservedMountPaths();
  EXPECT_TRUE(expected_paths == reserved_paths);

  manager_.ReserveMountPath(path1, MOUNT_ERROR_UNKNOWN_FILESYSTEM);
  reserved_paths = manager_.GetReservedMountPaths();
  expected_paths.insert(path1.value());
  EXPECT_TRUE(expected_paths == reserved_paths);

  manager_.ReserveMountPath(path2, MOUNT_ERROR_UNKNOWN_FILESYSTEM);
  reserved_paths = manager_.GetReservedMountPaths();
  expected_paths.insert(path2.value());
  EXPECT_TRUE(expected_paths == reserved_paths);

  manager_.UnreserveMountPath(path1);
  reserved_paths = manager_.GetReservedMountPaths();
  expected_paths.erase(path1.value());
  EXPECT_TRUE(expected_paths == reserved_paths);

  manager_.UnreserveMountPath(path2);
  reserved_paths = manager_.GetReservedMountPaths();
  expected_paths.erase(path2.value());
  EXPECT_TRUE(expected_paths == reserved_paths);
}

// Verifies that MountManager::ReserveMountPath() and
// MountManager::UnreserveMountPath() work as expected.
TEST_F(MountManagerTest, ReserveAndUnreserveMountPath) {
  mount_path_ = kMountPath;

  EXPECT_FALSE(manager_.IsMountPathReserved(mount_path_));
  EXPECT_EQ(MOUNT_ERROR_NONE, manager_.GetMountErrorOfReservedMountPath(
                                  base::FilePath(mount_path_)));
  manager_.ReserveMountPath(base::FilePath(mount_path_),
                            MOUNT_ERROR_UNKNOWN_FILESYSTEM);
  EXPECT_TRUE(manager_.IsMountPathReserved(mount_path_));
  EXPECT_EQ(
      MOUNT_ERROR_UNKNOWN_FILESYSTEM,
      manager_.GetMountErrorOfReservedMountPath(base::FilePath(mount_path_)));
  manager_.UnreserveMountPath(base::FilePath(mount_path_));
  EXPECT_FALSE(manager_.IsMountPathReserved(mount_path_));
  EXPECT_EQ(MOUNT_ERROR_NONE, manager_.GetMountErrorOfReservedMountPath(
                                  base::FilePath(mount_path_)));

  // Removing a nonexistent mount path should be ok
  manager_.UnreserveMountPath(base::FilePath(mount_path_));
  EXPECT_FALSE(manager_.IsMountPathReserved(mount_path_));

  // Adding an existent mount path should be ok
  manager_.ReserveMountPath(base::FilePath(mount_path_),
                            MOUNT_ERROR_UNSUPPORTED_FILESYSTEM);
  EXPECT_TRUE(manager_.IsMountPathReserved(mount_path_));
  EXPECT_EQ(
      MOUNT_ERROR_UNSUPPORTED_FILESYSTEM,
      manager_.GetMountErrorOfReservedMountPath(base::FilePath(mount_path_)));
  manager_.ReserveMountPath(base::FilePath(mount_path_),
                            MOUNT_ERROR_UNKNOWN_FILESYSTEM);
  EXPECT_TRUE(manager_.IsMountPathReserved(mount_path_));
  EXPECT_EQ(
      MOUNT_ERROR_UNSUPPORTED_FILESYSTEM,
      manager_.GetMountErrorOfReservedMountPath(base::FilePath(mount_path_)));
  manager_.UnreserveMountPath(base::FilePath(mount_path_));
  EXPECT_FALSE(manager_.IsMountPathReserved(mount_path_));
  EXPECT_EQ(MOUNT_ERROR_NONE, manager_.GetMountErrorOfReservedMountPath(
                                  base::FilePath(mount_path_)));
}

// Verifies that MountManager::GetMountEntries() returns the expected list of
// mount entries under different scenarios.
TEST_F(MountManagerTest, GetMountEntries) {
  // No mount entry is returned.
  std::vector<MountEntry> mount_entries = manager_.GetMountEntries();
  EXPECT_TRUE(mount_entries.empty());

  // A normal mount entry is returned.
  manager_.AddMountStateCache(kSourcePath, MakeMountPoint(kMountPath));
  mount_entries = manager_.GetMountEntries();
  ASSERT_EQ(1, mount_entries.size());
  EXPECT_EQ(MOUNT_ERROR_NONE, mount_entries[0].error_type);
  EXPECT_EQ(kSourcePath, mount_entries[0].source_path);
  EXPECT_EQ(MOUNT_SOURCE_REMOVABLE_DEVICE, mount_entries[0].source_type);
  EXPECT_EQ(kMountPath, mount_entries[0].mount_path);

  // A reserved mount entry is returned.
  manager_.ReserveMountPath(base::FilePath(kMountPath),
                            MOUNT_ERROR_UNKNOWN_FILESYSTEM);
  mount_entries = manager_.GetMountEntries();
  ASSERT_EQ(1, mount_entries.size());
  EXPECT_EQ(MOUNT_ERROR_UNKNOWN_FILESYSTEM, mount_entries[0].error_type);
  EXPECT_EQ(kSourcePath, mount_entries[0].source_path);
  EXPECT_EQ(MOUNT_SOURCE_REMOVABLE_DEVICE, mount_entries[0].source_type);
  EXPECT_EQ(kMountPath, mount_entries[0].mount_path);

  EXPECT_CALL(platform_, RemoveEmptyDirectory(kMountPath))
      .WillOnce(Return(false));
}

// Verifies that MountManager::IsPathImmediateChildOfParent() correctly
// determines if a path is an immediate child of another path.
TEST_F(MountManagerTest, IsPathImmediateChildOfParent) {
  EXPECT_TRUE(manager_.IsPathImmediateChildOfParent(
      base::FilePath("/media/archive/test.zip"),
      base::FilePath("/media/archive")));
  EXPECT_TRUE(manager_.IsPathImmediateChildOfParent(
      base::FilePath("/media/archive/test.zip/"),
      base::FilePath("/media/archive")));
  EXPECT_TRUE(manager_.IsPathImmediateChildOfParent(
      base::FilePath("/media/archive/test.zip"),
      base::FilePath("/media/archive/")));
  EXPECT_TRUE(manager_.IsPathImmediateChildOfParent(
      base::FilePath("/media/archive/test.zip/"),
      base::FilePath("/media/archive/")));
  EXPECT_FALSE(manager_.IsPathImmediateChildOfParent(
      base::FilePath("/media/archive/test.zip/doc.zip"),
      base::FilePath("/media/archive/")));
  EXPECT_FALSE(manager_.IsPathImmediateChildOfParent(
      base::FilePath("/media/archive/test.zip"),
      base::FilePath("/media/removable")));
  EXPECT_FALSE(manager_.IsPathImmediateChildOfParent(
      base::FilePath("/tmp/archive/test.zip"),
      base::FilePath("/media/removable")));
  EXPECT_FALSE(manager_.IsPathImmediateChildOfParent(
      base::FilePath("/media"), base::FilePath("/media/removable")));
  EXPECT_FALSE(manager_.IsPathImmediateChildOfParent(
      base::FilePath("/media/removable"), base::FilePath("/media/removable")));
  EXPECT_FALSE(manager_.IsPathImmediateChildOfParent(
      base::FilePath("/media/removable/"), base::FilePath("/media/removable")));
  EXPECT_FALSE(manager_.IsPathImmediateChildOfParent(
      base::FilePath("/media/removable/."),
      base::FilePath("/media/removable")));
  EXPECT_FALSE(manager_.IsPathImmediateChildOfParent(
      base::FilePath("/media/removable/.."),
      base::FilePath("/media/removable")));
}

// Verifies that MountManager::IsValidMountPath() correctly determines if a
// mount path is an immediate child of the mount root.
TEST_F(MountManagerTest, IsValidMountPath) {
  EXPECT_TRUE(
      manager_.IsValidMountPath(base::FilePath("/media/removable/test")));
  EXPECT_TRUE(
      manager_.IsValidMountPath(base::FilePath("/media/removable/test/")));
  EXPECT_TRUE(
      manager_.IsValidMountPath(base::FilePath("/media/removable/test/")));
  EXPECT_TRUE(
      manager_.IsValidMountPath(base::FilePath("/media/removable//test")));
  EXPECT_FALSE(
      manager_.IsValidMountPath(base::FilePath("/media/archive/test")));
  EXPECT_FALSE(manager_.IsValidMountPath(base::FilePath("/media/removable")));
  EXPECT_FALSE(manager_.IsValidMountPath(base::FilePath("/media/removable/")));
  EXPECT_FALSE(manager_.IsValidMountPath(base::FilePath("/media/removable/.")));
  EXPECT_FALSE(
      manager_.IsValidMountPath(base::FilePath("/media/removable/..")));
  EXPECT_FALSE(
      manager_.IsValidMountPath(base::FilePath("/media/removable/test/doc")));
  EXPECT_FALSE(
      manager_.IsValidMountPath(base::FilePath("/media/removable/../test")));
  EXPECT_FALSE(
      manager_.IsValidMountPath(base::FilePath("/media/removable/../test/")));
  EXPECT_FALSE(
      manager_.IsValidMountPath(base::FilePath("/media/removable/test/..")));
  EXPECT_FALSE(
      manager_.IsValidMountPath(base::FilePath("/media/removable/test/../")));
}

// Verifies that MountManager::Mount() returns an error when the source is
// not mounted yet but attempted to remount it.
TEST_F(MountManagerTest, RemountFailedNotMounted) {
  options_.push_back("remount");

  EXPECT_CALL(manager_, DoMount(_, _, _, _, _)).Times(0);

  // source_path = kSourcePath has not been mounted yet.
  manager_.Mount(kSourcePath, filesystem_type_, options_, GetMountCallback());
  EXPECT_TRUE(mount_completed_);
  EXPECT_EQ(MOUNT_ERROR_PATH_NOT_MOUNTED, mount_error_);
}

// Verifies that MountManager::Mount() returns no error when it successfully
// remounts a source path on a specified mount path.
TEST_F(MountManagerTest, RemountSucceededWithGivenSourcePath) {
  // Mount a device in read-write mode.
  base::FilePath mount_path(kMountPath);
  EXPECT_CALL(manager_, SuggestMountPath(_)).WillOnce(Return(kMountPath));
  EXPECT_CALL(platform_, CreateOrReuseEmptyDirectoryWithFallback(_, _, _))
      .WillOnce(Return(true));

  auto ptr = std::make_unique<MountPoint>(
      MountPointData{
          .mount_path = mount_path,
          .source = kSourcePath,
          .flags = 0,
      },
      &platform_);
  EXPECT_CALL(manager_,
              DoMount(kSourcePath, filesystem_type_, _, mount_path, _))
      .WillOnce(DoAll(SetArgPointee<4>(MOUNT_ERROR_NONE),
                      Return(ByMove(std::move(ptr)))));
  manager_.Mount(kSourcePath, filesystem_type_, {"rw"}, GetMountCallback());
  EXPECT_TRUE(mount_completed_);
  EXPECT_EQ(MOUNT_ERROR_NONE, mount_error_);
  EXPECT_EQ(kMountPath, mount_path_);

  {
    const std::optional<MountEntry> mount_entry =
        manager_.GetMountEntryForTest(kSourcePath);
    ASSERT_TRUE(mount_entry);
    EXPECT_FALSE(mount_entry->is_read_only);
    EXPECT_EQ(kMountPath, mount_entry->mount_path);
  }

  // Remount with read-only mount option.
  EXPECT_CALL(platform_, Mount(kSourcePath, kMountPath, filesystem_type_,
                               MS_RDONLY | MS_REMOUNT, _))
      .WillOnce(Return(MOUNT_ERROR_NONE));
  manager_.Mount(kSourcePath, filesystem_type_, {"remount", "ro"},
                 GetMountCallback());
  EXPECT_TRUE(mount_completed_);
  EXPECT_EQ(MOUNT_ERROR_NONE, mount_error_);
  EXPECT_EQ(kMountPath, mount_path_);
  EXPECT_TRUE(manager_.IsMountPathInCache(mount_path_));

  {
    const std::optional<MountEntry> mount_entry =
        manager_.GetMountEntryForTest(kSourcePath);
    EXPECT_TRUE(mount_entry);
    EXPECT_TRUE(mount_entry->is_read_only);
  }

  // Should be unmounted correctly even after remount.
  EXPECT_CALL(platform_, Unmount(kMountPath, _))
      .WillOnce(Return(MOUNT_ERROR_NONE));
  EXPECT_CALL(platform_, RemoveEmptyDirectory(kMountPath))
      .WillOnce(Return(true));
  EXPECT_TRUE(manager_.UnmountAll());
  EXPECT_FALSE(manager_.IsMountPathInCache(kMountPath));
  EXPECT_FALSE(manager_.IsMountPathReserved(kMountPath));
}

}  // namespace cros_disks
