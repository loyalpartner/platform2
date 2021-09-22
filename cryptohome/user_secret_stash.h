// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CRYPTOHOME_USER_SECRET_STASH_H_
#define CRYPTOHOME_USER_SECRET_STASH_H_

#include <base/optional.h>
#include <brillo/secure_blob.h>
#include <stdint.h>

#include <map>
#include <memory>
#include <string>

#include "cryptohome/flatbuffer_secure_allocator_bridge.h"
#include "cryptohome/user_secret_stash_container_generated.h"

namespace cryptohome {

// This wraps the UserSecretStash flatbuffer message, and is the only way that
// the UserSecretStash is accessed. Don't pass the raw flatbuffer around.
class UserSecretStash {
 public:
  // Container for a wrapped (encrypted) USS main key.
  struct WrappedKeyBlock {
    // The algorithm used for wrapping the USS main key.
    UserSecretStashEncryptionAlgorithm encryption_algorithm =
        UserSecretStashEncryptionAlgorithm::NONE;
    // This is the encrypted USS main key.
    brillo::SecureBlob encrypted_key;
    // The random IV used in the USS main key encryption.
    brillo::SecureBlob iv;
    // The GCM tag generated by the block cipher.
    brillo::SecureBlob gcm_tag;
  };

  // Sets up a UserSecretStash with a random file system key, and a random reset
  // secret.
  static std::unique_ptr<UserSecretStash> CreateRandom();
  // This deserializes the |flatbuffer| into a UserSecretStashContainer table.
  // Besides unencrypted data, that table contains a ciphertext, which is
  // decrypted with the |main_key| using AES-GCM-256. It doesn't return the
  // plaintext, it populates the fields of the class with the encrypted message.
  static std::unique_ptr<UserSecretStash> FromEncryptedContainer(
      const brillo::SecureBlob& flatbuffer, const brillo::SecureBlob& main_key);
  // Same as |FromEncryptedContainer()|, but the main key is unwrapped from the
  // USS container using the given wrapping key. The |main_key| output argument
  // is populated with the unwrapped main key on success.
  static std::unique_ptr<UserSecretStash> FromEncryptedContainerWithWrappingKey(
      const brillo::SecureBlob& flatbuffer,
      const std::string& wrapping_id,
      const brillo::SecureBlob& wrapping_key,
      brillo::SecureBlob* main_key);

  virtual ~UserSecretStash() = default;

  // Because this class contains raw secrets, it should never be copy-able.
  UserSecretStash(const UserSecretStash&) = delete;
  UserSecretStash& operator=(const UserSecretStash&) = delete;

  const brillo::SecureBlob& GetFileSystemKey() const;
  void SetFileSystemKey(const brillo::SecureBlob& key);

  const brillo::SecureBlob& GetResetSecret() const;
  void SetResetSecret(const brillo::SecureBlob& secret);

  // Returns whether there's a wrapped key block with the given wrapping ID.
  bool HasWrappedMainKey(const std::string& wrapping_id) const;
  // Unwraps (decrypts) the USS main key from the wrapped key block with the
  // given wrapping ID. Returns null if it doesn't exist or the unwrapping
  // fails.
  base::Optional<brillo::SecureBlob> UnwrapMainKey(
      const std::string& wrapping_id,
      const brillo::SecureBlob& wrapping_key) const;
  // Wraps (encrypts) the USS main key using the given wrapped key. The wrapped
  // data is added into the USS as a wrapped key block with the given wrapping
  // ID. |main_key| must be non-empty, and |wrapping_key| - of
  // |kAesGcm256KeySize| length. Returns false if the wrapping ID is already
  // used or the wrapping fails.
  bool AddWrappedMainKey(const brillo::SecureBlob& main_key,
                         const std::string& wrapping_id,
                         const brillo::SecureBlob& wrapping_key);
  // Removes the wrapped key with the given ID. If it doesn't exist, returns
  // false.
  bool RemoveWrappedMainKey(const std::string& wrapping_id);

  // This uses the |main_key|, which should be 256-bit as of right now, to
  // encrypt this UserSecretStash class. The object is converted to a
  // UserSecretStashPayload table, serialized, encrypted with AES-GCM-256, and
  // serialized as a UserSecretStashContainer table.
  base::Optional<brillo::SecureBlob> GetEncryptedContainer(
      const brillo::SecureBlob& main_key);

 private:
  // Decrypts the USS payload flatbuffer using the passed main key and
  // constructs the USS instance from it. Returns null on decryption or
  // validation failure.
  static std::unique_ptr<UserSecretStash> FromEncryptedPayload(
      const brillo::SecureBlob& ciphertext,
      const brillo::SecureBlob& iv,
      const brillo::SecureBlob& gcm_tag,
      const std::map<std::string, WrappedKeyBlock>& wrapped_key_blocks,
      const brillo::SecureBlob& main_key);

  UserSecretStash(const brillo::SecureBlob& file_system_key,
                  const brillo::SecureBlob& reset_secret);

  // A key registered with the kernel to decrypt files.
  brillo::SecureBlob file_system_key_;
  // The reset secret used for any PinWeaver backed credentials.
  brillo::SecureBlob reset_secret_;
  // Stores multiple wrapped (encrypted) representations of the main key, each
  // wrapped using a different intermediate key. The map's index is the wrapping
  // ID, which is an opaque string (although upper programmatic layers can add
  // semantics to it, in order to map it to the authentication method).
  std::map<std::string, WrappedKeyBlock> wrapped_key_blocks_;
};

}  // namespace cryptohome

#endif  // CRYPTOHOME_USER_SECRET_STASH_H_
