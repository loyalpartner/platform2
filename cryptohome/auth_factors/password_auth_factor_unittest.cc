// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Unit tests for PasswordAuthFactor.

#include "cryptohome/auth_factors/password_auth_factor.h"

#include <memory>
#include <string>
#include <utility>

#include <brillo/secure_blob.h>
#include <gtest/gtest.h>

#include "cryptohome/credentials.h"
#include "cryptohome/mock_keyset_management.h"

using ::testing::_;
using ::testing::ByMove;
using ::testing::DoAll;
using ::testing::Eq;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::SetArgPointee;

namespace cryptohome {

// Fake username to be used in this test suite.
const char kFakeUsername[] = "test_username";
const char kFakePassword[] = "test_pass";

class PasswordAuthFactorTest : public ::testing::Test {
 public:
  PasswordAuthFactorTest() = default;
  PasswordAuthFactorTest(const PasswordAuthFactorTest&) = delete;
  PasswordAuthFactorTest& operator=(const PasswordAuthFactorTest&) = delete;
  ~PasswordAuthFactorTest() override = default;

 protected:
  // Mock KeysetManagent object, will be passed to PasswordAuthFactorTest for
  // its internal use.
  NiceMock<MockKeysetManagement> keyset_management_;
};

TEST_F(PasswordAuthFactorTest, PersistentAuthenticateAuthFactorTest_Success) {
  // Setup
  auto vk = std::make_unique<VaultKeyset>();
  Credentials creds(kFakeUsername, brillo::SecureBlob(kFakePassword));
  EXPECT_CALL(keyset_management_, LoadUnwrappedKeyset(_, _))
      .WillOnce(DoAll(SetArgPointee<1>(MOUNT_ERROR_NONE),
                      Return(ByMove(std::move(vk)))));
  std::unique_ptr<AuthFactor> pass_auth_factor =
      std::make_unique<PasswordAuthFactor>(&keyset_management_);

  // Test
  EXPECT_THAT(
      pass_auth_factor->AuthenticateAuthFactor(creds, false /*ephemeral user*/),
      Eq(MOUNT_ERROR_NONE));

  // Verify
  std::unique_ptr<CredentialVerifier> verifier =
      pass_auth_factor->TakeCredentialVerifier();
  EXPECT_TRUE(verifier->Verify(brillo::SecureBlob(kFakePassword)));
}

TEST_F(PasswordAuthFactorTest, PersistentAuthenticateAuthFactorTest_Fail) {
  // Setup
  Credentials creds(kFakeUsername, brillo::SecureBlob(kFakePassword));
  EXPECT_CALL(keyset_management_, LoadUnwrappedKeyset(_, _))
      .WillOnce(
          DoAll(SetArgPointee<1>(MOUNT_ERROR_FATAL), Return(ByMove(nullptr))));
  std::unique_ptr<AuthFactor> pass_auth_factor =
      std::make_unique<PasswordAuthFactor>(&keyset_management_);

  // Test
  EXPECT_THAT(
      pass_auth_factor->AuthenticateAuthFactor(creds, false /*ephemeral user*/),
      Eq(MOUNT_ERROR_FATAL));
}

TEST_F(PasswordAuthFactorTest, EphemeralAuthenticateAuthFactorTest) {
  // Setup
  auto vk = std::make_unique<VaultKeyset>();
  Credentials creds(kFakeUsername, brillo::SecureBlob(kFakePassword));
  std::unique_ptr<AuthFactor> pass_auth_factor =
      std::make_unique<PasswordAuthFactor>(&keyset_management_);
  EXPECT_CALL(keyset_management_, LoadUnwrappedKeyset(_, _)).Times(0);

  // Test
  EXPECT_THAT(
      pass_auth_factor->AuthenticateAuthFactor(creds, true /*ephemeral user*/),
      Eq(MOUNT_ERROR_NONE));
  std::unique_ptr<CredentialVerifier> verifier =
      pass_auth_factor->TakeCredentialVerifier();

  // Verify
  EXPECT_TRUE(verifier->Verify(brillo::SecureBlob(kFakePassword)));
}

}  // namespace cryptohome
