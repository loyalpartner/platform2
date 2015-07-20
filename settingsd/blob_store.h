// Copyright 2015 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SETTINGSD_BLOB_STORE_H_
#define SETTINGSD_BLOB_STORE_H_

#include <stdint.h>
#include <string>
#include <vector>

#include <base/macros.h>

namespace settingsd {

// A class that loads and stores Blobs.
class BlobStore {
 public:
  class Handle {
   public:
    bool IsValid() const;

   private:
    Handle();
    Handle(unsigned int blob_id, const std::string& source_id);

    // 0 is considered to be an invalid blob id.
    const unsigned int blob_id_;
    const std::string source_id_;

    friend class BlobStore;
  };

 public:
  // |storage_path| needs to point to a directory that the system user running
  // settingsd has write access to. If the directory does not already exist, it
  // will be created on the first invocation of the Store() method.
  explicit BlobStore(const std::string& storage_path);

  // Stores the |blob| originating from the source identifid by |source_id| on
  // the disk.
  Handle Store(const std::string& source_id,
               const std::vector<uint8_t>& blob) const;

  // Loads the blob identified by |handle| from disk.
  const std::vector<uint8_t> Load(Handle handle) const;

  // Returns the list of handles to all documents provided by the source
  // identified by |source_id| in increasing order of blob id.
  std::vector<Handle> List(const std::string& source_id) const;

 private:
  // Path to the root of the directory hierarchy to store blobs in.
  const std::string storage_path_;

  // Constructs the path for blob with id |blob_id| for |source_id|. If either
  // |blob_id| or |source_id| are invalid (see implementation for comments),
  // this method fails and returns the empty string.
  std::string GetBlobPath(unsigned int blob_id,
                          const std::string& source_id) const;

  // Constructs the path containing the blobs for |source_id|. If |source_id| is
  // invalid (see implementation for comments), this method fails and returns
  // the empty string.
  std::string GetSourcePath(const std::string& source_id) const;

  // Attempts to extract the blob id from the filename. If |filename| does not
  // follow the naming scheme (see implementation for comments), this method
  // fails and returns 0.
  unsigned int FilenameToBlobId(const std::string& filename) const;

  // Returns the next unused blob id for |source_id|. Note that this function is
  // not safe against race conditions in cases where another process is trying
  // to find the next unused identifier as well.
  unsigned int GetNextUnusedBlobId(const std::string& source_id) const;

  DISALLOW_COPY_AND_ASSIGN(BlobStore);
};

}  // namespace settingsd

#endif  // SETTINGSD_BLOB_STORE_H_
