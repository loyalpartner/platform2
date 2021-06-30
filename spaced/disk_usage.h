// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SPACED_DISK_USAGE_H_
#define SPACED_DISK_USAGE_H_

#include <sys/statvfs.h>

#include <memory>
#include <utility>

#include <base/files/file_path.h>
#include <brillo/blkdev_utils/lvm.h>
#include <brillo/brillo_export.h>

namespace spaced {

class BRILLO_EXPORT DiskUsageUtil {
 public:
  DiskUsageUtil();
  virtual ~DiskUsageUtil();

  virtual uint64_t GetFreeDiskSpace(const base::FilePath& path);
  virtual uint64_t GetTotalDiskSpace(const base::FilePath& path);

 protected:
  // Runs statvfs() on a given path.
  virtual int StatVFS(const base::FilePath& path, struct statvfs* st);

  // Retrieves the stateful partition's thinpool.
  virtual base::Optional<brillo::Thinpool> GetThinpool();

 private:
  std::unique_ptr<brillo::LogicalVolumeManager> lvm_;
};

}  // namespace spaced

#endif  // SPACED_DISK_USAGE_H_