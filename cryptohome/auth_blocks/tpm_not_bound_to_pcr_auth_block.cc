// Copyright 2020 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cryptohome/auth_blocks/tpm_not_bound_to_pcr_auth_block.h"

#include <map>
#include <string>
#include <utility>
#include <variant>

#include <base/check.h>
#include <base/logging.h>
#include <brillo/secure_blob.h>
#include <libhwsec/status.h>
#include <libhwsec-foundation/crypto/aes.h>
#include <libhwsec-foundation/crypto/hmac.h>
#include <libhwsec-foundation/crypto/scrypt.h>
#include <libhwsec-foundation/crypto/secure_blob_util.h>

#include "cryptohome/auth_blocks/tpm_auth_block_utils.h"
#include "cryptohome/crypto.h"
#include "cryptohome/crypto_error.h"
#include "cryptohome/cryptohome_keys_manager.h"
#include "cryptohome/cryptohome_metrics.h"
#include "cryptohome/error/location_utils.h"
#include "cryptohome/tpm.h"
#include "cryptohome/vault_keyset.pb.h"

using cryptohome::error::CryptohomeCryptoError;
using cryptohome::error::ErrorAction;
using cryptohome::error::ErrorActionSet;
using hwsec::TPMErrorBase;
using hwsec::TPMRetryAction;
using hwsec_foundation::CreateSecureRandomBlob;
using hwsec_foundation::DeriveSecretsScrypt;
using hwsec_foundation::HmacSha256;
using hwsec_foundation::kAesBlockSize;
using hwsec_foundation::kDefaultAesKeySize;
using hwsec_foundation::kDefaultLegacyPasswordRounds;
using hwsec_foundation::kTpmDecryptMaxRetries;
using hwsec_foundation::PasskeyToAesKey;
using hwsec_foundation::status::MakeStatus;
using hwsec_foundation::status::OkStatus;
using hwsec_foundation::status::StatusChain;

namespace cryptohome {

TpmNotBoundToPcrAuthBlock::TpmNotBoundToPcrAuthBlock(
    Tpm* tpm, CryptohomeKeysManager* cryptohome_keys_manager)
    : SyncAuthBlock(kTpmBackedNonPcrBound),
      tpm_(tpm),
      cryptohome_key_loader_(
          cryptohome_keys_manager->GetKeyLoader(CryptohomeKeyType::kRSA)),
      utils_(tpm, cryptohome_key_loader_) {
  CHECK(tpm != nullptr);
  CHECK(cryptohome_key_loader_ != nullptr);
}

CryptoStatus TpmNotBoundToPcrAuthBlock::Derive(const AuthInput& auth_input,
                                               const AuthBlockState& state,
                                               KeyBlobs* key_out_data) {
  const TpmNotBoundToPcrAuthBlockState* tpm_state;
  if (!(tpm_state =
            std::get_if<TpmNotBoundToPcrAuthBlockState>(&state.state))) {
    LOG(ERROR) << "Invalid AuthBlockState";
    return MakeStatus<CryptohomeCryptoError>(
        CRYPTOHOME_ERR_LOC(
            kLocTpmNotBoundToPcrAuthBlockInvalidBlockStateInDerive),
        ErrorActionSet(
            {ErrorAction::kDevCheckUnexpectedState, ErrorAction::kAuth}),
        CryptoError::CE_OTHER_CRYPTO);
  }

  if (!tpm_state->salt.has_value()) {
    LOG(ERROR) << "Invalid TpmNotBoundToPcrAuthBlockState: missing salt";
    return MakeStatus<CryptohomeCryptoError>(
        CRYPTOHOME_ERR_LOC(kLocTpmNotBoundToPcrAuthBlockNoSaltInDerive),
        ErrorActionSet({ErrorAction::kDevCheckUnexpectedState,
                        ErrorAction::kAuth, ErrorAction::kDeleteVault}),
        CryptoError::CE_OTHER_CRYPTO);
  }
  if (!tpm_state->tpm_key.has_value()) {
    LOG(ERROR) << "Invalid TpmNotBoundToPcrAuthBlockState: missing tpm_key";
    return MakeStatus<CryptohomeCryptoError>(
        CRYPTOHOME_ERR_LOC(kLocTpmNotBoundToPcrAuthBlockNoTpmKeyInDerive),
        ErrorActionSet({ErrorAction::kDevCheckUnexpectedState,
                        ErrorAction::kAuth, ErrorAction::kDeleteVault}),
        CryptoError::CE_OTHER_CRYPTO);
  }
  if (!tpm_state->scrypt_derived.has_value()) {
    LOG(ERROR)
        << "Invalid TpmNotBoundToPcrAuthBlockState: missing scrypt_derived";
    return MakeStatus<CryptohomeCryptoError>(
        CRYPTOHOME_ERR_LOC(
            kLocTpmNotBoundToPcrAuthBlockNoScryptDerivedInDerive),
        ErrorActionSet({ErrorAction::kDevCheckUnexpectedState,
                        ErrorAction::kAuth, ErrorAction::kDeleteVault}),
        CryptoError::CE_OTHER_CRYPTO);
  }

  brillo::SecureBlob tpm_public_key_hash;
  if (tpm_state->tpm_public_key_hash.has_value()) {
    tpm_public_key_hash = tpm_state->tpm_public_key_hash.value();
  }

  CryptoStatus error = utils_.CheckTPMReadiness(
      tpm_state->tpm_key.has_value(),
      tpm_state->tpm_public_key_hash.has_value(), tpm_public_key_hash);
  if (!error.ok()) {
    return MakeStatus<CryptohomeCryptoError>(
               CRYPTOHOME_ERR_LOC(
                   kLocTpmNotBoundToPcrAuthBlockTpmNotReadyInDerive))
        .Wrap(std::move(error));
  }

  key_out_data->vkk_iv = brillo::SecureBlob(kAesBlockSize);
  key_out_data->vkk_key = brillo::SecureBlob(kDefaultAesKeySize);

  brillo::SecureBlob salt = tpm_state->salt.value();
  brillo::SecureBlob tpm_key = tpm_state->tpm_key.value();

  error = DecryptTpmNotBoundToPcr(*tpm_state, auth_input.user_input.value(),
                                  tpm_key, salt, &key_out_data->vkk_iv.value(),
                                  &key_out_data->vkk_key.value());
  if (!error.ok()) {
    if (!tpm_state->tpm_public_key_hash.has_value()) {
      return MakeStatus<CryptohomeCryptoError>(
          CRYPTOHOME_ERR_LOC(kLocTpmNotBoundToPcrAuthBlockNoPubKeyHashInDerive),
          ErrorActionSet(
              {ErrorAction::kDevCheckUnexpectedState, ErrorAction::kAuth}),
          CryptoError::CE_NO_PUBLIC_KEY_HASH);
    }
    return MakeStatus<CryptohomeCryptoError>(
               CRYPTOHOME_ERR_LOC(
                   kLocTpmNotBoundToPcrAuthBlockDecryptFailedInDerive),
               ErrorActionSet(
                   {ErrorAction::kIncorrectAuth, ErrorAction::kAuth}))
        .Wrap(std::move(error));
  }

  key_out_data->chaps_iv = key_out_data->vkk_iv;

  return OkStatus<CryptohomeCryptoError>();
}

CryptoStatus TpmNotBoundToPcrAuthBlock::Create(const AuthInput& user_input,
                                               AuthBlockState* auth_block_state,
                                               KeyBlobs* key_blobs) {
  const brillo::SecureBlob& vault_key = user_input.user_input.value();
  brillo::SecureBlob salt =
      CreateSecureRandomBlob(CRYPTOHOME_DEFAULT_KEY_SALT_SIZE);

  // If the cryptohome key isn't loaded, try to load it.
  if (!cryptohome_key_loader_->HasCryptohomeKey())
    cryptohome_key_loader_->Init();

  // If the key still isn't loaded, fail the operation.
  if (!cryptohome_key_loader_->HasCryptohomeKey()) {
    return MakeStatus<CryptohomeCryptoError>(
        CRYPTOHOME_ERR_LOC(
            kLocTpmNotBoundToPcrAuthBlockNoCryptohomeKeyInCreate),
        ErrorActionSet({ErrorAction::kReboot, ErrorAction::kRetry,
                        ErrorAction::kPowerwash}),
        CryptoError::CE_TPM_CRYPTO);
  }

  const auto local_blob = CreateSecureRandomBlob(kDefaultAesKeySize);
  brillo::SecureBlob tpm_key;
  brillo::SecureBlob aes_skey(kDefaultAesKeySize);
  brillo::SecureBlob kdf_skey(kDefaultAesKeySize);
  brillo::SecureBlob vkk_iv(kAesBlockSize);
  if (!DeriveSecretsScrypt(vault_key, salt, {&aes_skey, &kdf_skey, &vkk_iv})) {
    return MakeStatus<CryptohomeCryptoError>(
        CRYPTOHOME_ERR_LOC(
            kLocTpmNotBoundToPcrAuthBlockScryptDeriveFailedInCreate),
        ErrorActionSet({ErrorAction::kDevCheckUnexpectedState}),
        CryptoError::CE_OTHER_CRYPTO);
  }

  // Encrypt the VKK using the TPM and the user's passkey.  The output is an
  // encrypted blob in tpm_key, which is stored in the serialized vault
  // keyset.
  for (int i = 0; i < kTpmDecryptMaxRetries; i++) {
    hwsec::Status err =
        tpm_->EncryptBlob(cryptohome_key_loader_->GetCryptohomeKey(),
                          local_blob, aes_skey, &tpm_key);
    if (err == nullptr) {
      break;
    }

    if (!TpmAuthBlockUtils::TPMErrorIsRetriable(err)) {
      LOG(ERROR) << "Failed to wrap vkk with creds: " << err;
      return MakeStatus<CryptohomeCryptoError>(
                 CRYPTOHOME_ERR_LOC(
                     kLocTpmNotBoundToPcrAuthBlockEncryptFailedInCreate),
                 ErrorActionSet({ErrorAction::kReboot,
                                 ErrorAction::kDevCheckUnexpectedState}))
          .Wrap(TpmAuthBlockUtils::TPMErrorToCryptohomeCryptoError(
              std::move(err)));
    }

    // If the error is retriable, reload the key first.
    if (!cryptohome_key_loader_->ReloadCryptohomeKey()) {
      LOG(ERROR) << "Unable to reload Cryptohome key while createing "
                    "TpmNotBoundToPcrAuthBlock:"
                 << err;
      return MakeStatus<CryptohomeCryptoError>(
          CRYPTOHOME_ERR_LOC(
              kLocTpmNotBoundToPcrAuthBlockReloadKeyFailedInCreate),
          ErrorActionSet({ErrorAction::kReboot}), CryptoError::CE_TPM_REBOOT);
    }
  }

  TpmNotBoundToPcrAuthBlockState auth_state;
  // Allow this to fail.  It is not absolutely necessary; it allows us to
  // detect a TPM clear.  If this fails due to a transient issue, then on next
  // successful login, the vault keyset will be re-saved anyway.
  brillo::SecureBlob pub_key_hash;
  hwsec::Status err = tpm_->GetPublicKeyHash(
      cryptohome_key_loader_->GetCryptohomeKey(), &pub_key_hash);
  if (err != nullptr) {
    LOG(ERROR) << "Failed to get tpm public key hash: " << err;
  } else {
    auth_state.tpm_public_key_hash = pub_key_hash;
  }

  auth_state.scrypt_derived = true;
  auth_state.tpm_key = tpm_key;
  auth_state.salt = std::move(salt);

  // Pass back the vkk_key and vkk_iv so the generic secret wrapping can use it.
  key_blobs->vkk_key = HmacSha256(kdf_skey, local_blob);
  // Note that one might expect the IV to be part of the AuthBlockState. But
  // since it's taken from the scrypt output, it's actually created by the auth
  // block, not used to initialize the auth block.
  key_blobs->vkk_iv = vkk_iv;
  key_blobs->chaps_iv = vkk_iv;

  *auth_block_state = AuthBlockState{.state = std::move(auth_state)};
  return OkStatus<CryptohomeCryptoError>();
}

CryptoStatus TpmNotBoundToPcrAuthBlock::DecryptTpmNotBoundToPcr(
    const TpmNotBoundToPcrAuthBlockState& tpm_state,
    const brillo::SecureBlob& vault_key,
    const brillo::SecureBlob& tpm_key,
    const brillo::SecureBlob& salt,
    brillo::SecureBlob* vkk_iv,
    brillo::SecureBlob* vkk_key) const {
  brillo::SecureBlob aes_skey(kDefaultAesKeySize);
  brillo::SecureBlob kdf_skey(kDefaultAesKeySize);
  brillo::SecureBlob local_vault_key(vault_key.begin(), vault_key.end());
  unsigned int rounds = tpm_state.password_rounds.has_value()
                            ? tpm_state.password_rounds.value()
                            : kDefaultLegacyPasswordRounds;

  // TODO(b/204200132): check if this branch is unnecessary.
  if (tpm_state.scrypt_derived.value()) {
    if (!DeriveSecretsScrypt(vault_key, salt, {&aes_skey, &kdf_skey, vkk_iv})) {
      return MakeStatus<CryptohomeCryptoError>(
          CRYPTOHOME_ERR_LOC(
              kLocTpmNotBoundToPcrAuthBlockScryptDeriveFailedInDecrypt),
          ErrorActionSet({ErrorAction::kDevCheckUnexpectedState}),
          CryptoError::CE_OTHER_FATAL);
    }
  } else {
    PasskeyToAesKey(vault_key, salt, rounds, &aes_skey, NULL);
  }

  for (int i = 0; i < kTpmDecryptMaxRetries; i++) {
    hwsec::Status err =
        tpm_->DecryptBlob(cryptohome_key_loader_->GetCryptohomeKey(), tpm_key,
                          aes_skey, &local_vault_key);
    if (err == nullptr) {
      break;
    }

    if (!TpmAuthBlockUtils::TPMErrorIsRetriable(err)) {
      LOG(ERROR) << "Failed to unwrap VKK with creds: " << err;
      ReportCryptohomeError(kDecryptAttemptWithTpmKeyFailed);
      return MakeStatus<CryptohomeCryptoError>(
                 CRYPTOHOME_ERR_LOC(
                     kLocTpmNotBoundToPcrAuthBlockDecryptFailedInDecrypt),
                 ErrorActionSet({ErrorAction::kReboot,
                                 ErrorAction::kDevCheckUnexpectedState,
                                 ErrorAction::kAuth}),
                 CryptoError::CE_TPM_REBOOT)
          .Wrap(TpmAuthBlockUtils::TPMErrorToCryptohomeCryptoError(
              std::move(err)));
    }

    // If the error is retriable, reload the key first.
    if (!cryptohome_key_loader_->ReloadCryptohomeKey()) {
      LOG(ERROR) << "Unable to reload Cryptohome key while decrypting "
                    "TpmNotBoundToPcrAuthBlock:"
                 << err;
      ReportCryptohomeError(kDecryptAttemptWithTpmKeyFailed);
      return MakeStatus<CryptohomeCryptoError>(
          CRYPTOHOME_ERR_LOC(
              kLocTpmNotBoundToPcrAuthBlockReloadKeyFailedInDecrypt),
          ErrorActionSet({ErrorAction::kReboot}), CryptoError::CE_TPM_REBOOT);
    }
  }

  // TODO(zuan): Handle cases in which all retries failed.

  // TODO(b/204200132): check if this branch is unnecessary.
  if (tpm_state.scrypt_derived.value()) {
    *vkk_key = HmacSha256(kdf_skey, local_vault_key);
  } else {
    if (!PasskeyToAesKey(local_vault_key, salt, rounds, vkk_key, vkk_iv)) {
      LOG(ERROR) << "Failure converting IVKK to VKK.";
      return MakeStatus<CryptohomeCryptoError>(
          CRYPTOHOME_ERR_LOC(
              kLocTpmNotBoundToPcrAuthBlockVKKConversionFailedInDecrypt),
          ErrorActionSet({ErrorAction::kDevCheckUnexpectedState}),
          CryptoError::CE_OTHER_FATAL);
    }
  }
  return OkStatus<CryptohomeCryptoError>();
}

}  // namespace cryptohome
