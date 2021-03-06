// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CRYPTOHOME_AUTH_BLOCKS_ASYNC_CHALLENGE_CREDENTIAL_AUTH_BLOCK_H_
#define CRYPTOHOME_AUTH_BLOCKS_ASYNC_CHALLENGE_CREDENTIAL_AUTH_BLOCK_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <base/memory/weak_ptr.h>

#include "cryptohome/auth_blocks/auth_block_state.h"
#include "cryptohome/auth_blocks/libscrypt_compat_auth_block.h"
#include "cryptohome/challenge_credentials/challenge_credentials_helper.h"
#include "cryptohome/credentials.h"
#include "cryptohome/key_challenge_service.h"

namespace cryptohome {

// The asynchronous auth block for challenge credential.
// Note: Create/Derive cannot be called twice after we instantiate this auth
// block.
class AsyncChallengeCredentialAuthBlock : public AuthBlock {
 public:
  AsyncChallengeCredentialAuthBlock(
      Tpm* tpm,
      ChallengeCredentialsHelper* challenge_credentials_helper,
      std::unique_ptr<KeyChallengeService> key_challenge_service,
      const std::string& account_id);
  ~AsyncChallengeCredentialAuthBlock() = default;

  // This creates the KeyBlobs & AuthBlockState  from the key challenge service.
  void Create(const AuthInput& user_input, CreateCallback callback) override;

  // This derives the KeyBlobs from the key challenge service.
  void Derive(const AuthInput& user_input,
              const AuthBlockState& state,
              DeriveCallback callback) override;

 private:
  // This continues the creating process after generated the new high entropy
  // secret from the key challenge service.
  void CreateContinue(CreateCallback callback,
                      std::unique_ptr<structure::SignatureChallengeInfo>
                          signature_challenge_info,
                      std::unique_ptr<brillo::SecureBlob> passkey);

  // This continues the deriving process after decrypted the high entropy secret
  // from the key challenge service.
  void DeriveContinue(DeriveCallback callback,
                      const AuthBlockState& scrypt_state,
                      std::unique_ptr<brillo::SecureBlob> passkey);

  Tpm* const tpm_;
  ChallengeCredentialsHelper* const challenge_credentials_helper_;
  std::unique_ptr<KeyChallengeService> key_challenge_service_;
  const std::string account_id_;

  base::WeakPtrFactory<AsyncChallengeCredentialAuthBlock> weak_factory_{this};
};

}  // namespace cryptohome

#endif  // CRYPTOHOME_AUTH_BLOCKS_ASYNC_CHALLENGE_CREDENTIAL_AUTH_BLOCK_H_
