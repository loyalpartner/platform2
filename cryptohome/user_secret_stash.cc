// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cryptohome/user_secret_stash.h"

#include <base/logging.h>
#include <brillo/secure_blob.h>
#include <stdint.h>

#include <memory>
#include <string>

#include "cryptohome/crypto/aes.h"
#include "cryptohome/crypto/secure_blob_util.h"
#include "cryptohome/cryptohome_common.h"
#include "cryptohome/flatbuffer_secure_allocator_bridge.h"
#include "cryptohome/user_secret_stash_container_generated.h"
#include "cryptohome/user_secret_stash_payload_generated.h"

namespace cryptohome {

namespace {

brillo::SecureBlob GenerateAesGcmEncryptedUSS(
    const brillo::SecureBlob& ciphertext,
    const brillo::SecureBlob& tag,
    const brillo::SecureBlob& iv) {
  std::unique_ptr<flatbuffers::Allocator> allocator =
      std::make_unique<FlatbufferSecureAllocatorBridge>();
  flatbuffers::FlatBufferBuilder builder(4096, allocator.get(),
                                         /*own_allocator=*/false);

  auto ciphertext_vector =
      builder.CreateVector(ciphertext.data(), ciphertext.size());
  auto tag_vector = builder.CreateVector(tag.data(), tag.size());
  auto iv_vector = builder.CreateVector(iv.data(), iv.size());

  UserSecretStashContainerBuilder uss_container_builder(builder);
  uss_container_builder.add_encryption_algorithm(
      UserSecretStashEncryptionAlgorithm::AES_GCM_256);
  uss_container_builder.add_ciphertext(ciphertext_vector);
  uss_container_builder.add_aes_gcm_tag(tag_vector);
  uss_container_builder.add_iv(iv_vector);
  auto uss_container = uss_container_builder.Finish();

  builder.Finish(uss_container);

  auto ret_val =
      brillo::SecureBlob(builder.GetBufferPointer(),
                         builder.GetBufferPointer() + builder.GetSize());

  builder.Clear();

  return ret_val;
}

}  // namespace

const brillo::SecureBlob& UserSecretStash::GetFileSystemKey() const {
  return file_system_key_.value();
}

void UserSecretStash::SetFileSystemKey(const brillo::SecureBlob& key) {
  file_system_key_ = key;
}

const brillo::SecureBlob& UserSecretStash::GetResetSecret() const {
  return reset_secret_.value();
}

void UserSecretStash::SetResetSecret(const brillo::SecureBlob& secret) {
  reset_secret_ = secret;
}

void UserSecretStash::InitializeRandom() {
  file_system_key_ =
      CreateSecureRandomBlob(CRYPTOHOME_DEFAULT_512_BIT_KEY_SIZE);
  reset_secret_ = CreateSecureRandomBlob(CRYPTOHOME_RESET_SECRET_LENGTH);
}

base::Optional<brillo::SecureBlob> UserSecretStash::GetEncryptedContainer(
    const brillo::SecureBlob& main_key) {
  std::unique_ptr<flatbuffers::Allocator> allocator =
      std::make_unique<FlatbufferSecureAllocatorBridge>();
  flatbuffers::FlatBufferBuilder builder(4096, allocator.get(),
                                         /*own_allocator=*/false);

  auto fs_key_vector =
      builder.CreateVector(file_system_key_->data(), file_system_key_->size());
  auto reset_secret_vector =
      builder.CreateVector(reset_secret_->data(), reset_secret_->size());

  UserSecretStashPayloadBuilder uss_builder(builder);
  uss_builder.add_file_system_key(fs_key_vector);
  uss_builder.add_reset_secret(reset_secret_vector);
  auto uss = uss_builder.Finish();

  builder.Finish(uss);

  brillo::SecureBlob serialized_uss(
      builder.GetBufferPointer(),
      builder.GetBufferPointer() + builder.GetSize());

  brillo::SecureBlob tag, iv, ciphertext;
  if (!AesGcmEncrypt(serialized_uss, /*ad=*/base::nullopt, main_key, &iv, &tag,
                     &ciphertext)) {
    LOG(ERROR) << "Failed to encrypt UserSecretStash";
    return base::nullopt;
  }

  builder.Clear();

  return base::Optional<brillo::SecureBlob>(
      GenerateAesGcmEncryptedUSS(ciphertext, tag, iv));
}

bool UserSecretStash::FromEncryptedContainer(
    const brillo::SecureBlob& flatbuffer, const brillo::SecureBlob& main_key) {
  flatbuffers::Verifier aes_verifier(flatbuffer.data(), flatbuffer.size());
  if (!VerifyUserSecretStashContainerBuffer(aes_verifier)) {
    LOG(ERROR) << "The UserSecretStashContainer flatbuffer is invalid";
    return false;
  }

  auto uss_container = GetUserSecretStashContainer(flatbuffer.data());
  if (!flatbuffers::IsFieldPresent(
          uss_container, UserSecretStashContainer::VT_ENCRYPTION_ALGORITHM) ||
      !flatbuffers::IsFieldPresent(uss_container,
                                   UserSecretStashContainer::VT_CIPHERTEXT) ||
      !flatbuffers::IsFieldPresent(uss_container,
                                   UserSecretStashContainer::VT_IV) ||
      !flatbuffers::IsFieldPresent(uss_container,
                                   UserSecretStashContainer::VT_AES_GCM_TAG)) {
    LOG(ERROR) << "UserSecretStashContainer is missing fields";
    return false;
  }

  UserSecretStashEncryptionAlgorithm algorithm =
      uss_container->encryption_algorithm();
  if (algorithm != UserSecretStashEncryptionAlgorithm::AES_GCM_256) {
    LOG(ERROR) << "UserSecretStashContainer uses unknown algorithm: "
               << static_cast<int>(algorithm);
    return false;
  }

  brillo::SecureBlob ciphertext(uss_container->ciphertext()->begin(),
                                uss_container->ciphertext()->end());
  brillo::SecureBlob iv(uss_container->iv()->begin(),
                        uss_container->iv()->end());
  brillo::SecureBlob tag(uss_container->aes_gcm_tag()->begin(),
                         uss_container->aes_gcm_tag()->end());

  if (ciphertext.empty() || iv.empty() || tag.empty()) {
    LOG(ERROR) << "UserSecretStashContainer has empty fields";
    return false;
  }

  brillo::SecureBlob serialized_uss;
  if (!AesGcmDecrypt(ciphertext, /*ad=*/base::nullopt, tag, main_key, iv,
                     &serialized_uss)) {
    LOG(ERROR) << "Failed to decrypt UserSecretStash";
    return false;
  }

  flatbuffers::Verifier uss_verifier(serialized_uss.data(),
                                     serialized_uss.size());
  if (!VerifyUserSecretStashPayloadBuffer(uss_verifier)) {
    LOG(ERROR) << "The UserSecretStashPayload flatbuffer is invalid";
    return false;
  }

  auto uss = GetUserSecretStashPayload(serialized_uss.data());

  brillo::SecureBlob file_system_key;
  if (flatbuffers::IsFieldPresent(uss,
                                  UserSecretStashPayload::VT_FILE_SYSTEM_KEY)) {
    file_system_key = brillo::SecureBlob(uss->file_system_key()->begin(),
                                         uss->file_system_key()->end());
  }
  if (!file_system_key.empty()) {
    file_system_key_ = file_system_key;
  }

  brillo::SecureBlob reset_secret;
  if (flatbuffers::IsFieldPresent(uss,
                                  UserSecretStashPayload::VT_RESET_SECRET)) {
    reset_secret = brillo::SecureBlob(uss->reset_secret()->begin(),
                                      uss->reset_secret()->end());
  }
  if (!reset_secret.empty()) {
    reset_secret_ = reset_secret;
  }

  return true;
}

}  // namespace cryptohome
