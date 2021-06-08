// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cryptohome/crypto/recovery_crypto.h"

#include <algorithm>
#include <utility>
#include <vector>

#include <base/logging.h>
#include <base/stl_util.h>
#include <brillo/secure_blob.h>

#include "cryptohome/crypto/big_num_util.h"
#include "cryptohome/crypto/ecdh_hkdf.h"
#include "cryptohome/crypto/elliptic_curve.h"
#include "cryptohome/crypto/error_util.h"
#include "cryptohome/cryptolib.h"

namespace cryptohome {

const EllipticCurve::CurveType RecoveryCrypto::kCurve =
    EllipticCurve::CurveType::kPrime256;

const HkdfHash RecoveryCrypto::kHkdfHash = HkdfHash::kSha256;

// Size of public/private key for EllipticCurve::CurveType::kPrime256.
constexpr size_t kEc256PubKeySize = 65;
constexpr size_t kEc256PrivKeySize = 32;

namespace {

// Cryptographic operations for cryptohome recovery performed on CPU (software
// emulation).
class RecoveryCryptoImpl : public RecoveryCrypto {
 public:
  explicit RecoveryCryptoImpl(EllipticCurve ec);

  ~RecoveryCryptoImpl() override;

  bool GenerateShares(const brillo::SecureBlob& mediator_pub_key,
                      const brillo::SecureBlob& hkdf_info,
                      const brillo::SecureBlob& hkdf_salt,
                      EncryptedMediatorShare* encrypted_mediator_share,
                      brillo::SecureBlob* destination_share,
                      brillo::SecureBlob* dealer_pub_key) const override;
  bool GeneratePublisherKeys(const brillo::SecureBlob& dealer_pub_key,
                             brillo::SecureBlob* publisher_pub_key,
                             brillo::SecureBlob* publisher_dh) const override;
  bool RecoverDestination(const brillo::SecureBlob& publisher_pub_key,
                          const brillo::SecureBlob& destination_share,
                          const brillo::SecureBlob& mediated_publisher_pub_key,
                          brillo::SecureBlob* destination_dh) const override;

 private:
  // Encrypts mediator share and stores as `encrypted_ms` with
  // embedded ephemeral public key, AES-GCM tag and iv. Returns false if error
  // occurred.
  bool EncryptMediatorShare(const brillo::SecureBlob& mediator_pub_key,
                            const brillo::SecureBlob& hkdf_info,
                            const brillo::SecureBlob& hkdf_salt,
                            const brillo::SecureBlob& mediator_share,
                            EncryptedMediatorShare* encrypted_ms,
                            BN_CTX* context) const;

  EllipticCurve ec_;
};

RecoveryCryptoImpl::RecoveryCryptoImpl(EllipticCurve ec) : ec_(std::move(ec)) {}

RecoveryCryptoImpl::~RecoveryCryptoImpl() = default;

bool RecoveryCryptoImpl::EncryptMediatorShare(
    const brillo::SecureBlob& mediator_pub_key,
    const brillo::SecureBlob& hkdf_info,
    const brillo::SecureBlob& hkdf_salt,
    const brillo::SecureBlob& mediator_share,
    RecoveryCrypto::EncryptedMediatorShare* encrypted_ms,
    BN_CTX* context) const {
  brillo::SecureBlob ephemeral_priv_key;
  if (!ec_.GenerateKeysAsSecureBlobs(&encrypted_ms->ephemeral_pub_key,
                                     &ephemeral_priv_key, context)) {
    LOG(ERROR) << "Failed to generate EC keys";
    return false;
  }

  brillo::SecureBlob aes_gcm_key;
  if (!GenerateEcdhHkdfSenderKey(ec_, mediator_pub_key,
                                 encrypted_ms->ephemeral_pub_key,
                                 ephemeral_priv_key, hkdf_info, hkdf_salt,
                                 kHkdfHash, kAesGcm256KeySize, &aes_gcm_key)) {
    LOG(ERROR) << "Failed to generate ECDH+HKDF sender keys";
    return false;
  }

  // Dispose private key.
  ephemeral_priv_key.clear();

  if (!CryptoLib::AesGcmEncrypt(mediator_share, aes_gcm_key, &encrypted_ms->iv,
                                &encrypted_ms->tag,
                                &encrypted_ms->encrypted_data)) {
    LOG(ERROR) << "Failed to perform AES-GCM encryption";
    return false;
  }

  return true;
}

bool RecoveryCryptoImpl::GenerateShares(
    const brillo::SecureBlob& mediator_pub_key,
    const brillo::SecureBlob& hkdf_info,
    const brillo::SecureBlob& hkdf_salt,
    EncryptedMediatorShare* encrypted_mediator_share,
    brillo::SecureBlob* destination_share,
    brillo::SecureBlob* dealer_pub_key) const {
  ScopedBN_CTX context = CreateBigNumContext();
  if (!context.get()) {
    LOG(ERROR) << "Failed to allocate BN_CTX structure";
    return false;
  }

  // Generate two shares and a secret equal to the sum.
  // Loop until the sum of two shares is non-zero (modulo order).
  crypto::ScopedBIGNUM secret;
  crypto::ScopedBIGNUM destination_share_bn =
      ec_.RandomNonZeroScalar(context.get());
  if (!destination_share_bn) {
    LOG(ERROR) << "Failed to generate secret";
    return false;
  }
  crypto::ScopedBIGNUM mediator_share_bn;
  do {
    mediator_share_bn = ec_.RandomNonZeroScalar(context.get());
    if (!mediator_share_bn) {
      LOG(ERROR) << "Failed to generate secret";
      return false;
    }
    secret =
        ec_.ModAdd(*mediator_share_bn, *destination_share_bn, context.get());
    if (!secret) {
      LOG(ERROR) << "Failed to perform modulo addition";
      return false;
    }
  } while (BN_is_zero(secret.get()));

  crypto::ScopedEC_POINT dealer_pub_point =
      ec_.MultiplyWithGenerator(*secret, context.get());
  if (!dealer_pub_point) {
    LOG(ERROR) << "Failed to perform MultiplyWithGenerator operation";
    return false;
  }
  brillo::SecureBlob mediator_share;
  if (!BigNumToSecureBlob(*mediator_share_bn, ec_.ScalarSizeInBytes(),
                          &mediator_share)) {
    LOG(ERROR) << "Failed to convert BIGNUM to SecureBlob";
    return false;
  }
  if (!BigNumToSecureBlob(*destination_share_bn, ec_.ScalarSizeInBytes(),
                          destination_share)) {
    LOG(ERROR) << "Failed to convert BIGNUM to SecureBlob";
    return false;
  }
  if (!ec_.PointToSecureBlob(*dealer_pub_point, dealer_pub_key,
                             context.get())) {
    LOG(ERROR) << "Failed to convert EC_POINT to SecureBlob";
    return false;
  }
  if (!EncryptMediatorShare(mediator_pub_key, hkdf_info, hkdf_salt,
                            mediator_share, encrypted_mediator_share,
                            context.get())) {
    LOG(ERROR) << "Failed to encrypt mediator share";
    return false;
  }
  return true;
}

bool RecoveryCryptoImpl::GeneratePublisherKeys(
    const brillo::SecureBlob& dealer_pub_key,
    brillo::SecureBlob* publisher_pub_key,
    brillo::SecureBlob* publisher_dh) const {
  ScopedBN_CTX context = CreateBigNumContext();
  if (!context.get()) {
    LOG(ERROR) << "Failed to allocate BN_CTX structure";
    return false;
  }
  crypto::ScopedBIGNUM secret = ec_.RandomNonZeroScalar(context.get());
  if (!secret) {
    LOG(ERROR) << "Failed to generate secret";
    return false;
  }
  crypto::ScopedEC_POINT publisher_pub_point =
      ec_.MultiplyWithGenerator(*secret, context.get());
  if (!publisher_pub_point) {
    LOG(ERROR) << "Failed to perform MultiplyWithGenerator operation";
    return false;
  }
  crypto::ScopedEC_POINT dealer_pub_point =
      ec_.SecureBlobToPoint(dealer_pub_key, context.get());
  if (!dealer_pub_point) {
    LOG(ERROR) << "Failed to convert SecureBlob to EC_point";
    return false;
  }
  crypto::ScopedEC_POINT point_dh =
      ec_.Multiply(*dealer_pub_point, *secret, context.get());
  if (!point_dh) {
    LOG(ERROR) << "Failed to perform point multiplication";
    return false;
  }
  if (!ec_.PointToSecureBlob(*publisher_pub_point, publisher_pub_key,
                             context.get())) {
    LOG(ERROR) << "Failed to convert EC_POINT to SecureBlob";
    return false;
  }
  if (!ec_.PointToSecureBlob(*point_dh, publisher_dh, context.get())) {
    LOG(ERROR) << "Failed to convert EC_POINT to SecureBlob";
    return false;
  }
  return true;
}

bool RecoveryCryptoImpl::RecoverDestination(
    const brillo::SecureBlob& publisher_pub_key,
    const brillo::SecureBlob& destination_share,
    const brillo::SecureBlob& mediated_publisher_pub_key,
    brillo::SecureBlob* destination_dh) const {
  ScopedBN_CTX context = CreateBigNumContext();
  if (!context.get()) {
    LOG(ERROR) << "Failed to allocate BN_CTX structure";
    return false;
  }
  crypto::ScopedBIGNUM destination_share_bn =
      SecureBlobToBigNum(destination_share);
  if (!destination_share_bn) {
    LOG(ERROR) << "Failed to convert SecureBlob to BIGNUM";
    return false;
  }
  crypto::ScopedEC_POINT publisher_pub_point =
      ec_.SecureBlobToPoint(publisher_pub_key, context.get());
  if (!publisher_pub_point) {
    LOG(ERROR) << "Failed to convert SecureBlob to EC_POINT";
    return false;
  }
  crypto::ScopedEC_POINT mediated_publisher_pub_point =
      ec_.SecureBlobToPoint(mediated_publisher_pub_key, context.get());
  if (!mediated_publisher_pub_point) {
    LOG(ERROR) << "Failed to convert SecureBlob to EC_POINT";
    return false;
  }
  // Performs scalar multiplication of publisher_pub_key and destination_share.
  crypto::ScopedEC_POINT point_dh =
      ec_.Multiply(*publisher_pub_point, *destination_share_bn, context.get());
  if (!point_dh) {
    LOG(ERROR) << "Failed to perform scalar multiplication";
    return false;
  }
  crypto::ScopedEC_POINT point_dest =
      ec_.Add(*point_dh, *mediated_publisher_pub_point, context.get());
  if (!point_dest) {
    LOG(ERROR) << "Failed to perform point addition";
    return false;
  }
  if (!ec_.PointToSecureBlob(*point_dest, destination_dh, context.get())) {
    LOG(ERROR) << "Failed to convert EC_POINT to SecureBlob";
    return false;
  }
  return true;
}

// Appends `src_blob` to `dst_blob`.
void AppendToSecureBlob(const brillo::SecureBlob& src_blob,
                        brillo::SecureBlob* dst_blob) {
  dst_blob->insert(dst_blob->end(), src_blob.begin(), src_blob.end());
}

// Copies SecureBlob chunk of given size `chunk_size` starting at iterator `it`
// to `dst_blob`. Returns iterator pointing to first byte after the copied
// chunk.
brillo::SecureBlob::const_iterator CopySecureBlobChunk(
    const brillo::SecureBlob::const_iterator& it,
    size_t chunk_size,
    brillo::SecureBlob* dst_blob) {
  dst_blob->assign(it, it + chunk_size);
  return it + chunk_size;
}

}  // namespace

std::unique_ptr<RecoveryCrypto> RecoveryCrypto::Create() {
  ScopedBN_CTX context = CreateBigNumContext();
  if (!context.get()) {
    LOG(ERROR) << "Failed to allocate BN_CTX structure";
    return nullptr;
  }
  base::Optional<EllipticCurve> ec =
      EllipticCurve::Create(kCurve, context.get());
  if (!ec) {
    LOG(ERROR) << "Failed to create EllipticCurve";
    return nullptr;
  }
  return std::make_unique<RecoveryCryptoImpl>(std::move(*ec));
}

RecoveryCrypto::~RecoveryCrypto() = default;

bool RecoveryCrypto::SerializeEncryptedMediatorShareForTesting(
    const EncryptedMediatorShare& encrypted_mediator_share,
    brillo::SecureBlob* serialized_blob) {
  if (encrypted_mediator_share.tag.size() != kAesGcmTagSize) {
    LOG(ERROR) << "Invalid tag size in encrypted mediator share";
    return false;
  }
  if (encrypted_mediator_share.iv.size() != kAesGcmIVSize) {
    LOG(ERROR) << "Invalid iv size in encrypted mediator share";
    return false;
  }
  if (encrypted_mediator_share.ephemeral_pub_key.size() != kEc256PubKeySize) {
    LOG(ERROR)
        << "Invalid ephemeral public key size in encrypted mediator share";
    return false;
  }
  if (encrypted_mediator_share.encrypted_data.size() != kEc256PrivKeySize) {
    LOG(ERROR)
        << "Invalid ephemeral public key size in encrypted mediator share";
    return false;
  }
  serialized_blob->clear();
  serialized_blob->reserve(kAesGcmTagSize + kAesGcmIVSize + kEc256PubKeySize +
                           kEc256PrivKeySize);
  AppendToSecureBlob(encrypted_mediator_share.tag, serialized_blob);
  AppendToSecureBlob(encrypted_mediator_share.iv, serialized_blob);
  AppendToSecureBlob(encrypted_mediator_share.ephemeral_pub_key,
                     serialized_blob);
  AppendToSecureBlob(encrypted_mediator_share.encrypted_data, serialized_blob);
  return true;
}

bool RecoveryCrypto::DeserializeEncryptedMediatorShareForTesting(
    const brillo::SecureBlob& serialized_blob,
    EncryptedMediatorShare* encrypted_mediator_share) {
  if (serialized_blob.size() !=
      kAesGcmTagSize + kAesGcmIVSize + kEc256PubKeySize + kEc256PrivKeySize) {
    LOG(ERROR) << "Invalid size of serialized encrypted mediator share";
    return false;
  }
  auto it = serialized_blob.begin();
  it = CopySecureBlobChunk(it, kAesGcmTagSize, &encrypted_mediator_share->tag);
  it = CopySecureBlobChunk(it, kAesGcmIVSize, &encrypted_mediator_share->iv);
  it = CopySecureBlobChunk(it, kEc256PubKeySize,
                           &encrypted_mediator_share->ephemeral_pub_key);
  it = CopySecureBlobChunk(it, kEc256PrivKeySize,
                           &encrypted_mediator_share->encrypted_data);
  DCHECK(it == serialized_blob.end());
  return true;
}

}  // namespace cryptohome