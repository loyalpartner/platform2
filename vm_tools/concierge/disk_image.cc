// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <archive.h>
#include <archive_entry.h>
#include <unistd.h>

#include <algorithm>
#include <memory>
#include <utility>

#include <base/files/file_util.h>
#include <base/guid.h>
#include <base/memory/ptr_util.h>
#include <base/strings/stringprintf.h>

#include "vm_tools/concierge/disk_image.h"

namespace {

constexpr gid_t kPluginVmGid = 20128;

}  // namespace

namespace vm_tools {
namespace concierge {

DiskImageOperation::DiskImageOperation()
    : uuid_(base::GenerateGUID()),
      status_(DISK_STATUS_FAILED),
      source_size_(0),
      processed_size_(0) {
  CHECK(base::IsValidGUID(uuid_));
}

void DiskImageOperation::Run(uint64_t io_limit) {
  if (ExecuteIo(io_limit)) {
    Finalize();
  }
}

int DiskImageOperation::GetProgress() const {
  if (status() == DISK_STATUS_IN_PROGRESS) {
    if (source_size_ == 0)
      return 0;  // We do not know any better.

    return processed_size_ * 100 / source_size_;
  }

  // Any other status indicates completed operation (successfully or not)
  // so return 100%.
  return 100;
}

std::unique_ptr<PluginVmExportOperation> PluginVmExportOperation::Create(
    const base::FilePath disk_path, base::ScopedFD fd) {
  auto op = base::WrapUnique(
      new PluginVmExportOperation(std::move(disk_path), std::move(fd)));

  if (op->PrepareInput() && op->PrepareOutput()) {
    op->set_status(DISK_STATUS_IN_PROGRESS);
  }

  return op;
}

PluginVmExportOperation::PluginVmExportOperation(const base::FilePath disk_path,
                                                 base::ScopedFD out_fd)
    : src_image_path_(std::move(disk_path)),
      out_fd_(std::move(out_fd)),
      copying_data_(false) {
  set_source_size(ComputeDirectorySize(src_image_path_));
}

bool PluginVmExportOperation::PrepareInput() {
  in_ = ArchiveReader(archive_read_disk_new());
  if (!in_) {
    set_failure_reason("libarchive: failed to create reader");
    return false;
  }

  // Do not cross mount points.
  archive_read_disk_set_behavior(in_.get(),
                                 ARCHIVE_READDISK_NO_TRAVERSE_MOUNTS);
  // Do not traverse symlinks.
  archive_read_disk_set_symlink_physical(in_.get());

  int ret = archive_read_disk_open(in_.get(), src_image_path_.value().c_str());
  if (ret != ARCHIVE_OK) {
    set_failure_reason("failed to open source directory as an archive");
    return false;
  }

  return true;
}

bool PluginVmExportOperation::PrepareOutput() {
  out_ = ArchiveWriter(archive_write_new());
  if (!out_) {
    set_failure_reason("libarchive: failed to create writer");
    return false;
  }

  int ret = archive_write_set_format_zip(out_.get());
  if (ret != ARCHIVE_OK) {
    set_failure_reason(
        base::StringPrintf("libarchive: failed to initialize zip format: %s",
                           archive_error_string(out_.get())));
    return false;
  }

  ret = archive_write_open_fd(out_.get(), out_fd_.get());
  if (ret != ARCHIVE_OK) {
    set_failure_reason("failed to open output archive");
    return false;
  }

  return true;
}

void PluginVmExportOperation::MarkFailed(const char* msg, struct archive* a) {
  set_status(DISK_STATUS_FAILED);

  if (a) {
    set_failure_reason(
        base::StringPrintf("%s: %s", msg, archive_error_string(a)));
  } else {
    set_failure_reason(msg);
  }

  LOG(ERROR) << "PluginVm export failed: " << failure_reason();

  // Release resources.
  out_.reset();
  out_fd_.reset();
  in_.reset();
}

bool PluginVmExportOperation::ExecuteIo(uint64_t io_limit) {
  do {
    if (!copying_data_) {
      struct archive_entry* entry;
      int ret = archive_read_next_header(in_.get(), &entry);
      if (ret == ARCHIVE_EOF) {
        // Successfully copied entire archive.
        return true;
      }

      if (ret < ARCHIVE_OK) {
        MarkFailed("failed to read header", in_.get());
        break;
      }

      // Signal our intent to descend into directory (noop if current entry
      // is not a directory).
      archive_read_disk_descend(in_.get());

      const char* c_path = archive_entry_pathname(entry);
      if (!c_path || c_path[0] == '\0') {
        MarkFailed("archive entry read from disk has empty file name", NULL);
        break;
      }

      base::FilePath path(c_path);
      if (path == src_image_path_) {
        // Skip the image directory entry itself, as we will be storing
        // and restoring relative paths.
        continue;
      }

      // Strip the leading directory data as we want relative path.
      base::FilePath dest_path;
      if (!src_image_path_.AppendRelativePath(path, &dest_path)) {
        MarkFailed("failed to transform archive entry name", NULL);
        break;
      }
      archive_entry_set_pathname(entry, dest_path.value().c_str());

      ret = archive_write_header(out_.get(), entry);
      if (ret != ARCHIVE_OK) {
        MarkFailed("failed to write header", out_.get());
        break;
      }

      copying_data_ = archive_entry_size(entry) > 0;
    }

    if (copying_data_) {
      uint64_t bytes_read = CopyEntry(io_limit);
      io_limit -= std::min(bytes_read, io_limit);
      AccumulateProcessedSize(bytes_read);
    }

    if (!copying_data_) {
      int ret = archive_write_finish_entry(out_.get());
      if (ret != ARCHIVE_OK) {
        MarkFailed("failed to finish entry", out_.get());
        break;
      }
    }
  } while (io_limit > 0);

  // More copying is to be done (or there was a failure).
  return false;
}

uint64_t PluginVmExportOperation::CopyEntry(uint64_t io_limit) {
  uint64_t bytes_read = 0;

  do {
    uint8_t buf[16384];
    int count = archive_read_data(in_.get(), buf, sizeof(buf));
    if (count == 0) {
      // No more data
      copying_data_ = false;
      break;
    }

    if (count < 0) {
      MarkFailed("failed to read data block", in_.get());
      break;
    }

    bytes_read += count;

    int ret = archive_write_data(out_.get(), buf, count);
    if (ret < ARCHIVE_OK) {
      MarkFailed("failed to write data block", out_.get());
      break;
    }
  } while (bytes_read < io_limit);

  return bytes_read;
}

void PluginVmExportOperation::Finalize() {
  archive_read_close(in_.get());
  // Free the input archive.
  in_.reset();

  int ret = archive_write_close(out_.get());
  if (ret != ARCHIVE_OK) {
    MarkFailed("libarchive: failed to close writer", out_.get());
    return;
  }
  // Free the output archive structures.
  out_.reset();
  // Close the file descriptor.
  out_fd_.reset();

  set_status(DISK_STATUS_CREATED);
}

std::unique_ptr<PluginVmImportOperation> PluginVmImportOperation::Create(
    base::ScopedFD fd, const base::FilePath disk_path, uint64_t source_size) {
  auto op = base::WrapUnique(new PluginVmImportOperation(
      std::move(fd), source_size, std::move(disk_path)));

  if (op->PrepareInput() && op->PrepareOutput()) {
    op->set_status(DISK_STATUS_IN_PROGRESS);
  }

  return op;
}

PluginVmImportOperation::PluginVmImportOperation(base::ScopedFD in_fd,
                                                 uint64_t source_size,
                                                 const base::FilePath disk_path)
    : dest_image_path_(std::move(disk_path)),
      in_fd_(std::move(in_fd)),
      copying_data_(false) {
  set_source_size(source_size);
}

bool PluginVmImportOperation::PrepareInput() {
  in_ = ArchiveReader(archive_read_new());
  if (!in_.get()) {
    set_failure_reason("libarchive: failed to create reader");
    return false;
  }

  int ret = archive_read_support_format_zip(in_.get());
  if (ret != ARCHIVE_OK) {
    set_failure_reason("libarchive: failed to initialize zip format");
    return false;
  }

  ret = archive_read_support_filter_all(in_.get());
  if (ret != ARCHIVE_OK) {
    set_failure_reason("libarchive: failed to initialize filter");
    return false;
  }

  ret = archive_read_open_fd(in_.get(), in_fd_.get(), 102400);
  if (ret != ARCHIVE_OK) {
    set_failure_reason("failed to open input archive");
    return false;
  }

  return true;
}

bool PluginVmImportOperation::PrepareOutput() {
  // We are not using CreateUniqueTempDirUnderPath() because we want
  // to be able to identify images that are being imported, and that
  // requires directory name to not be random.
  base::FilePath disk_path(dest_image_path_.AddExtension(".tmp"));
  base::File::Error dir_error;
  if (!base::CreateDirectoryAndGetError(disk_path, &dir_error)) {
    set_failure_reason(std::string("failed to create output directory: ") +
                       base::File::ErrorToString(dir_error));
    return false;
  }

  CHECK(output_dir_.Set(disk_path));

  out_ = ArchiveWriter(archive_write_disk_new());
  if (!out_) {
    set_failure_reason("libarchive: failed to create writer");
    return false;
  }

  int ret = archive_write_disk_set_options(
      out_.get(), ARCHIVE_EXTRACT_SECURE_SYMLINKS |
                      ARCHIVE_EXTRACT_SECURE_NODOTDOT | ARCHIVE_EXTRACT_OWNER);
  if (ret != ARCHIVE_OK) {
    set_failure_reason("libarchive: failed to initialize filter");
    return false;
  }

  return true;
}

void PluginVmImportOperation::MarkFailed(const char* msg, struct archive* a) {
  set_status(DISK_STATUS_FAILED);

  if (a) {
    set_failure_reason(
        base::StringPrintf("%s: %s", msg, archive_error_string(a)));
  } else {
    set_failure_reason(msg);
  }

  LOG(ERROR) << "PluginVm import failed: " << failure_reason();

  // Release resources.
  out_.reset();
  if (!output_dir_.Delete()) {
    LOG(WARNING) << "Failed to delete output directory on error";
  }

  in_.reset();
  in_fd_.reset();
}

bool PluginVmImportOperation::ExecuteIo(uint64_t io_limit) {
  do {
    if (!copying_data_) {
      struct archive_entry* entry;
      int ret = archive_read_next_header(in_.get(), &entry);
      if (ret == ARCHIVE_EOF) {
        // Successfully copied entire archive.
        return true;
      }

      if (ret < ARCHIVE_OK) {
        MarkFailed("failed to read header", in_.get());
        break;
      }

      const char* c_path = archive_entry_pathname(entry);
      if (!c_path || c_path[0] == '\0') {
        MarkFailed("archive entry has empty file name", NULL);
        break;
      }

      base::FilePath path(c_path);
      if (path.empty() || path.IsAbsolute() || path.ReferencesParent()) {
        MarkFailed(
            "archive entry has invalid/absolute/referencing parent file name",
            NULL);
        break;
      }

      auto dest_path = output_dir_.GetPath().Append(path);
      archive_entry_set_pathname(entry, dest_path.value().c_str());

      archive_entry_set_uid(entry, getuid());
      archive_entry_set_gid(entry, kPluginVmGid);

      mode_t mode = archive_entry_filetype(entry);
      switch (mode) {
        case AE_IFREG:
          archive_entry_set_perm(entry, 0660);
          break;
        case AE_IFDIR:
          archive_entry_set_perm(entry, 0770);
          break;
      }

      ret = archive_write_header(out_.get(), entry);
      if (ret != ARCHIVE_OK) {
        MarkFailed("failed to write header", out_.get());
        break;
      }

      copying_data_ = archive_entry_size(entry) > 0;
    }

    if (copying_data_) {
      uint64_t bytes_read = CopyEntry(io_limit);
      io_limit -= std::min(bytes_read, io_limit);
      AccumulateProcessedSize(bytes_read);
    }

    if (!copying_data_) {
      int ret = archive_write_finish_entry(out_.get());
      if (ret != ARCHIVE_OK) {
        MarkFailed("failed to finish entry", out_.get());
        break;
      }
    }
  } while (io_limit > 0);

  // More copying is to be done (or there was a failure).
  return false;
}

// Note that this is extremely similar to PluginVmExportOperation::CopyEntry()
// implementation. The difference is the disk writer supports
// archive_write_data_block() API that handles sparse files, whereas generic
// writer does not, so we have to use separate implementations.
uint64_t PluginVmImportOperation::CopyEntry(uint64_t io_limit) {
  uint64_t bytes_read_begin = archive_filter_bytes(in_.get(), -1);
  uint64_t bytes_read = 0;

  do {
    const void* buff;
    size_t size;
    la_int64_t offset;
    int ret = archive_read_data_block(in_.get(), &buff, &size, &offset);
    if (ret == ARCHIVE_EOF) {
      copying_data_ = false;
      break;
    }

    if (ret != ARCHIVE_OK) {
      MarkFailed("failed to read data block", in_.get());
      break;
    }

    bytes_read = archive_filter_bytes(in_.get(), -1) - bytes_read_begin;

    ret = archive_write_data_block(out_.get(), buff, size, offset);
    if (ret != ARCHIVE_OK) {
      MarkFailed("failed to write data block", out_.get());
      break;
    }
  } while (bytes_read < io_limit);

  return bytes_read;
}

void PluginVmImportOperation::Finalize() {
  archive_read_close(in_.get());
  // Free the input archive.
  in_.reset();
  // Close the file descriptor.
  in_fd_.reset();

  int ret = archive_write_close(out_.get());
  if (ret != ARCHIVE_OK) {
    MarkFailed("libarchive: failed to close writer", out_.get());
    return;
  }
  // Free the output archive structures.
  out_.reset();
  // Make sure resulting image is accessible by the dispatcher process.
  if (chown(output_dir_.GetPath().value().c_str(), -1, kPluginVmGid) < 0) {
    MarkFailed("failed to change group of the destination directory", NULL);
    return;
  }
  if (chmod(output_dir_.GetPath().value().c_str(), 0770) < 0) {
    MarkFailed("failed to change permissions of the destination directory",
               NULL);
    return;
  }
  // Drop the ".tmp" suffix from the directory so that we recognize
  // it as a valid Plugin VM image.
  if (!base::Move(output_dir_.GetPath(), dest_image_path_)) {
    MarkFailed("Unable to rename resulting image directory", NULL);
    return;
  }
  // Tell it not to try cleaning up as we are committed to using the
  // image.
  output_dir_.Take();

  set_status(DISK_STATUS_CREATED);
}

}  // namespace concierge
}  // namespace vm_tools
