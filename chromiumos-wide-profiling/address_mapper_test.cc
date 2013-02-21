// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "address_mapper.h"

#include <gtest/gtest.h>

#include <base/logging.h>
#include <base/memory/scoped_ptr.h>

namespace {

struct Range {
  uint64 addr;
  uint64 size;
  bool contains(const uint64 check_addr) const {
    return (check_addr >= addr && check_addr < addr + size);
  }
};

// Some address ranges to map.
const Range kMapRanges[] = {
  { 0xff000000,   0x100000 },
  { 0x00a00000,    0x10000 },
  { 0x0c000000,  0x1000000 },
  { 0x00001000,    0x30000 },
};

// List of real addresses that are not in the above ranges.
const uint64 kAddressesNotInRanges[] = {
         0x0,
       0x100,
     0x38000,
     0x88888,
    0x100000,
    0x4fffff,
    0xa20000,
    0xcc0000,
    0xffffff,
   0x3e00000,
   0xb000000,
   0xd100000,
   0xfffffff,
  0x1fffffff,
  0x7ffffff0,
  0xdffffff0,
  0xfe000000,
  0xffffffff,
};

// A huge region that overlaps with all ranges in |kMapRanges|.
const Range kBigRegion = { 0xa00, 0xff000000 };

// A region that extends to the end of the address space.
const Range kEndRegion = { 0xffffffff00000000, 0x100000000 };

// A region toward the end of address space that overruns the end of the address
// space.
const Range kOutOfBoundsRegion = { 0xffffffff00000000, 0x200000000 };

// Number of regularly-spaced intervals within a mapped range to test.
const int kNumRangeTestIntervals = 8;

// A simple test function to convert a real address to a mapped address.
// Address ranges in |ranges| are mapped starting at address 0.
uint64 GetMappedAddressFromRanges(const Range* ranges,
                                  const unsigned int num_ranges,
                                  const uint64 addr) {
  unsigned int i;
  uint64 mapped_range_addr;
  for (i = 0, mapped_range_addr = 0;
       i < num_ranges;
       mapped_range_addr += ranges[i].size, ++i) {
    const Range& range = ranges[i];
    if (range.contains(addr))
      return (addr - range.addr) + mapped_range_addr;
  }
  return static_cast<uint64>(-1);
}

}  // namespace

// The unit test class for AddressMapper.
class AddressMapperTest : public ::testing::Test {
 public:
  AddressMapperTest() {}
  ~AddressMapperTest() {}

  virtual void SetUp() {
    mapper_.reset(new AddressMapper);
  }

 protected:
  // Maps a range using the AddressMapper and makes sure that it was successful.
  bool MapRange(const Range& range, bool remove_old_mappings) {
    LOG(INFO) << "Mapping range at " << (void*) range.addr
              << " with length of " << (void*) range.size;
    return mapper_->Map(range.addr, range.size, remove_old_mappings);
  }

  // Tests a range that has been mapped.  |expected_mapped_addr| is the starting
  // address that it should have been mapped to.  This mapper will test the
  // start and end addresses of the range, as well as a bunch of addresses
  // inside it.
  void TestMappedRange(const Range& range, const uint64 expected_mapped_addr) {
    uint64 mapped_addr = kuint64max;

    LOG(INFO) << "Testing range at " << (void*) range.addr << " with length of "
              << (void*) range.size;

    // Check address at the beginning of the range and at subsequent intervals.
    for (uint64 offset = 0;
         offset < range.size;
         offset += range.size / kNumRangeTestIntervals) {
      EXPECT_TRUE(mapper_->GetMappedAddress(range.addr + offset, &mapped_addr));
      EXPECT_EQ(expected_mapped_addr + offset, mapped_addr);
    }

    // Check address at end of the range.
    EXPECT_TRUE(mapper_->GetMappedAddress(range.addr + range.size - 1,
                                          &mapped_addr));
    EXPECT_EQ(expected_mapped_addr + range.size - 1, mapped_addr);
  }

  scoped_ptr<AddressMapper> mapper_;
};

// Map one range at a time and test looking up addresses.
TEST_F(AddressMapperTest, MapSingle) {
  for (unsigned int i = 0; i < arraysize(kMapRanges); ++i) {
    mapper_.reset(new AddressMapper);
    const Range& range = kMapRanges[i];
    ASSERT_TRUE(MapRange(range, false));
    EXPECT_EQ(1, mapper_->GetNumMappedRanges());
    TestMappedRange(range, 0);

    // Check addresses before the mapped range, should be invalid.
    uint64 mapped_addr;
    EXPECT_FALSE(mapper_->GetMappedAddress(range.addr - 1, &mapped_addr));
    EXPECT_FALSE(mapper_->GetMappedAddress(range.addr - 0x100, &mapped_addr));
    EXPECT_FALSE(mapper_->GetMappedAddress(range.addr + range.size,
                                           &mapped_addr));
    EXPECT_FALSE(mapper_->GetMappedAddress(range.addr + range.size + 0x100,
                                           &mapped_addr));
  }
}

// Map all the ranges at once and test looking up addresses.
TEST_F(AddressMapperTest, MapAll) {
  unsigned int i;
  for (i = 0; i < arraysize(kMapRanges); ++i)
    ASSERT_TRUE(MapRange(kMapRanges[i], false));
  EXPECT_EQ(arraysize(kMapRanges), mapper_->GetNumMappedRanges());

  // For each mapped range, test addresses at the start, middle, and end.
  // Also test the address right before and after each range.
  uint64 mapped_addr;
  for (i = 0; i < arraysize(kMapRanges); ++i) {
    const Range& range = kMapRanges[i];
    TestMappedRange(range,
                    GetMappedAddressFromRanges(kMapRanges,
                                               arraysize(kMapRanges),
                                               range.addr));

    // Check addresses before and after the mapped range, should be invalid.
    EXPECT_FALSE(mapper_->GetMappedAddress(range.addr - 1, &mapped_addr));
    EXPECT_FALSE(mapper_->GetMappedAddress(range.addr - 0x100, &mapped_addr));
    EXPECT_FALSE(mapper_->GetMappedAddress(range.addr + range.size,
                                           &mapped_addr));
    EXPECT_FALSE(mapper_->GetMappedAddress(range.addr + range.size + 0x100,
                                           &mapped_addr));
  }

  // Test some addresses that are out of these ranges, should not be able to
  // get mapped addresses.
  for (i = 0; i < arraysize(kAddressesNotInRanges); ++i) {
    EXPECT_FALSE(mapper_->GetMappedAddress(kAddressesNotInRanges[i],
                 &mapped_addr));
  }
}

// Test overlap detection.
TEST_F(AddressMapperTest, OverlapSimple) {
  unsigned int i;
  // Map all the ranges first.
  for (i = 0; i < arraysize(kMapRanges); ++i)
    ASSERT_TRUE(MapRange(kMapRanges[i], false));

  // Attempt to re-map each range, but offset by size / 2.
  for (i = 0; i < arraysize(kMapRanges); ++i) {
    Range range;
    range.addr = kMapRanges[i].addr + kMapRanges[i].size / 2;
    range.size = kMapRanges[i].size;
    // The maps should fail because of overlap with an existing mapping.
    EXPECT_FALSE(MapRange(range, false));
  }

  // Re-map each range with the same offset.  Only this time, remove any old
  // mapped range that overlaps with it.
  for (i = 0; i < arraysize(kMapRanges); ++i) {
    Range range;
    range.addr = kMapRanges[i].addr + kMapRanges[i].size / 2;
    range.size = kMapRanges[i].size;
    EXPECT_TRUE(MapRange(range, true));
    // Make sure the number of ranges is unchanged (one deleted, one added).
    EXPECT_EQ(arraysize(kMapRanges), mapper_->GetNumMappedRanges());

    // The range is shifted in real space but should still be the same in
    // quipper space.
    TestMappedRange(range,
                    GetMappedAddressFromRanges(kMapRanges,
                                               arraysize(kMapRanges),
                                               kMapRanges[i].addr));
  }
}

// Test mapping of a giant map that overlaps with all existing ranges.
TEST_F(AddressMapperTest, OverlapBig) {
  unsigned int i;
  // Map all the ranges first.
  for (i = 0; i < arraysize(kMapRanges); ++i)
    ASSERT_TRUE(MapRange(kMapRanges[i], false));

  // Make sure overlap is detected before removing old ranges.
  ASSERT_FALSE(MapRange(kBigRegion, false));
  ASSERT_TRUE(MapRange(kBigRegion, true));
  EXPECT_EQ(1, mapper_->GetNumMappedRanges());

  TestMappedRange(kBigRegion, 0);

  // Given the list of previously unmapped addresses, test that the ones within
  // |kBigRegion| are now mapped; for the ones that are not, test that they are
  // not mapped.
  for (i = 0; i < arraysize(kAddressesNotInRanges); ++i) {
    uint64 addr = kAddressesNotInRanges[i];
    uint64 mapped_addr = kuint64max;
    bool map_success = mapper_->GetMappedAddress(addr, &mapped_addr);
    if (kBigRegion.contains(addr)) {
      EXPECT_TRUE(map_success);
      EXPECT_EQ(addr - kBigRegion.addr, mapped_addr);
    } else {
      EXPECT_FALSE(map_success);
    }
  }

  // Check that addresses in the originally mapped ranges no longer map to the
  // same addresses if they fall within |kBigRegion|, and don't map at all if
  // they are not within |kBigRegion|.
  for (i = 0; i < arraysize(kMapRanges); ++i) {
    const Range& range = kMapRanges[i];
    for (uint64 addr = range.addr;
         addr < range.addr + range.size;
         addr += range.size / kNumRangeTestIntervals) {
      uint64 mapped_addr = kuint64max;
      bool map_success = mapper_->GetMappedAddress(addr, &mapped_addr);
      if (kBigRegion.contains(addr)) {
        EXPECT_TRUE(map_success);
        EXPECT_EQ(addr - kBigRegion.addr, mapped_addr);
      } else {
        EXPECT_FALSE(map_success);
      }
    }
  }
}

TEST_F(AddressMapperTest, EndOfMemory) {
  ASSERT_TRUE(MapRange(kEndRegion, true));
  EXPECT_EQ(1, mapper_->GetNumMappedRanges());
  TestMappedRange(kEndRegion, 0);
}

// Test mapping of an out-of-bounds mapping.
TEST_F(AddressMapperTest, OutOfBounds) {
  ASSERT_FALSE(MapRange(kOutOfBoundsRegion, false));
  ASSERT_FALSE(MapRange(kOutOfBoundsRegion, true));
  EXPECT_EQ(0, mapper_->GetNumMappedRanges());
  uint64 mapped_addr;
  EXPECT_FALSE(
      mapper_->GetMappedAddress(kOutOfBoundsRegion.addr + 0x100, &mapped_addr));
}

int main(int argc, char * argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
