// Copyright (c) 2009 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "update_engine/payload_generator/extent_utils.h"

#include <string>
#include <utility>
#include <vector>

#include <base/logging.h>
#include <base/macros.h>

#include "update_engine/extent_ranges.h"
#include "update_engine/payload_constants.h"
#include "update_engine/payload_generator/annotated_operation.h"

using std::string;
using std::vector;

namespace chromeos_update_engine {

void AppendBlockToExtents(vector<Extent>* extents, uint64_t block) {
  // First try to extend the last extent in |extents|, if any.
  if (!extents->empty()) {
    Extent& extent = extents->back();
    uint64_t next_block = extent.start_block() == kSparseHole ?
        kSparseHole : extent.start_block() + extent.num_blocks();
    if (next_block == block) {
      extent.set_num_blocks(extent.num_blocks() + 1);
      return;
    }
  }
  // If unable to extend the last extent, append a new single-block extent.
  Extent new_extent;
  new_extent.set_start_block(block);
  new_extent.set_num_blocks(1);
  extents->push_back(new_extent);
}

Extent GetElement(const vector<Extent>& collection, size_t index) {
  return collection[index];
}
Extent GetElement(
    const google::protobuf::RepeatedPtrField<Extent>& collection,
    size_t index) {
  return collection.Get(index);
}

void NormalizeExtents(vector<Extent>* extents) {
  vector<Extent> new_extents;
  for (const Extent& curr_ext : *extents) {
    if (new_extents.empty()) {
      new_extents.push_back(curr_ext);
      continue;
    }
    Extent& last_ext = new_extents.back();
    if (last_ext.start_block() + last_ext.num_blocks() ==
        curr_ext.start_block()) {
      // If the extents are touching, we want to combine them.
      last_ext.set_num_blocks(last_ext.num_blocks() + curr_ext.num_blocks());
    } else {
      // Otherwise just include the extent as is.
      new_extents.push_back(curr_ext);
    }
  }
  *extents = new_extents;
}

vector<Extent> ExtentsSublist(const vector<Extent>& extents,
                              uint64_t block_offset, uint64_t block_count) {
  vector<Extent> result;
  uint64_t scanned_blocks = 0;
  if (block_count == 0)
    return result;
  uint64_t end_block_offset = block_offset + block_count;
  for (const Extent& extent : extents) {
    // The loop invariant is that if |extents| has enough blocks, there's
    // still some extent to add to |result|. This implies that at the beginning
    // of the loop scanned_blocks < block_offset + block_count.
    if (scanned_blocks + extent.num_blocks() > block_offset) {
      // This case implies that |extent| has some overlapping with the requested
      // subsequence.
      uint64_t new_start = extent.start_block();
      uint64_t new_num_blocks = extent.num_blocks();
      if (scanned_blocks + new_num_blocks > end_block_offset) {
        // Cut the end part of the extent.
        new_num_blocks = end_block_offset - scanned_blocks;
      }
      if (block_offset > scanned_blocks) {
        // Cut the begin part of the extent.
        new_num_blocks -= block_offset - scanned_blocks;
        new_start += block_offset - scanned_blocks;
      }
      result.push_back(ExtentForRange(new_start, new_num_blocks));
    }
    scanned_blocks += extent.num_blocks();
    if (scanned_blocks >= end_block_offset)
      break;
  }
  return result;
}

bool operator==(const Extent& a, const Extent& b) {
  return a.start_block() == b.start_block() && a.num_blocks() == b.num_blocks();
}

}  // namespace chromeos_update_engine
