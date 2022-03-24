// Copyright 2022 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "libhwsec/error/tpm_manager_error.h"

#include <type_traits>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <libhwsec-foundation/error/testing_helper.h>

#include "libhwsec/status.h"

namespace hwsec {

using ::hwsec_foundation::error::testing::IsOk;
using ::hwsec_foundation::error::testing::NotOk;
using ::hwsec_foundation::status::MakeStatus;
using ::hwsec_foundation::status::StatusChain;
using ::tpm_manager::TpmManagerStatus;

class TestingTPMManagerErrorTest : public ::testing::Test {
 public:
  TestingTPMManagerErrorTest() {}
  ~TestingTPMManagerErrorTest() override = default;
};

TEST_F(TestingTPMManagerErrorTest, MakeStatus) {
  Status status = MakeStatus<TPMManagerError>(TpmManagerStatus::STATUS_SUCCESS);
  EXPECT_THAT(status, IsOk());

  status = MakeStatus<TPMManagerError>(TpmManagerStatus::STATUS_DEVICE_ERROR);
  EXPECT_THAT(status, NotOk());
}

TEST_F(TestingTPMManagerErrorTest, TPMRetryAction) {
  Status status =
      MakeStatus<TPMManagerError>(TpmManagerStatus::STATUS_DBUS_ERROR);
  EXPECT_EQ(status->ToTPMRetryAction(), TPMRetryAction::kCommunication);

  Status status2 = MakeStatus<TPMError>("OuO+").Wrap(std::move(status));
  EXPECT_EQ("OuO+: TpmManager status 3 (STATUS_DBUS_ERROR)",
            status2.ToFullString());
  EXPECT_EQ(status2->ToTPMRetryAction(), TPMRetryAction::kCommunication);

  EXPECT_EQ(MakeStatus<TPMManagerError>(TpmManagerStatus::STATUS_DEVICE_ERROR)
                ->ToTPMRetryAction(),
            TPMRetryAction::kReboot);
}

}  // namespace hwsec