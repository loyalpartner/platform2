// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cryptohome/cryptorecovery/recovery_crypto.h"

#include <algorithm>
#include <utility>
#include <vector>

#include <base/logging.h>
#include <base/stl_util.h>
#include <brillo/secure_blob.h>

#include "cryptohome/crypto/aes.h"
#include "cryptohome/crypto/big_num_util.h"
#include "cryptohome/crypto/ecdh_hkdf.h"
#include "cryptohome/crypto/elliptic_curve.h"
#include "cryptohome/crypto/error_util.h"
#include "cryptohome/crypto/secure_blob_util.h"
#include "cryptohome/cryptohome_common.h"
#include "cryptohome/cryptorecovery/recovery_crypto_hsm_cbor_serialization.h"
#include "cryptohome/cryptorecovery/recovery_crypto_util.h"

namespace cryptohome {
namespace cryptorecovery {

namespace {

brillo::SecureBlob GetRecoveryKeyHkdfInfo() {
  return brillo::SecureBlob("CryptoHome Wrapping Key");
}

brillo::SecureBlob GetMediatorShareHkdfInfo() {
  return brillo::SecureBlob(RecoveryCrypto::kMediatorShareHkdfInfoValue);
}

brillo::SecureBlob GetRequestPayloadPlainTextHkdfInfo() {
  return brillo::SecureBlob(
      RecoveryCrypto::kRequestPayloadPlainTextHkdfInfoValue);
}

brillo::SecureBlob GetResponsePayloadPlainTextHkdfInfo() {
  return brillo::SecureBlob(
      RecoveryCrypto::kResponsePayloadPlainTextHkdfInfoValue);
}

}  // namespace

const char RecoveryCrypto::kMediatorShareHkdfInfoValue[] = "HSM-Payload Key";

const char RecoveryCrypto::kRequestPayloadPlainTextHkdfInfoValue[] =
    "REQUEST-Payload Key";

const char RecoveryCrypto::kResponsePayloadPlainTextHkdfInfoValue[] =
    "RESPONSE-Payload Key";

const EllipticCurve::CurveType RecoveryCrypto::kCurve =
    EllipticCurve::CurveType::kPrime256;

const HkdfHash RecoveryCrypto::kHkdfHash = HkdfHash::kSha256;

const unsigned int RecoveryCrypto::kHkdfSaltLength = 32;

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

  bool GenerateRecoveryRequest(
      const HsmPayload& hsm_payload,
      const brillo::SecureBlob& request_meta_data,
      const brillo::SecureBlob& channel_priv_key,
      const brillo::SecureBlob& channel_pub_key,
      const brillo::SecureBlob& epoch_pub_key,
      brillo::SecureBlob* recovery_request,
      brillo::SecureBlob* ephemeral_pub_key) const override;
  bool GenerateHsmPayload(const brillo::SecureBlob& mediator_pub_key,
                          const brillo::SecureBlob& rsa_pub_key,
                          const brillo::SecureBlob& onboarding_metadata,
                          HsmPayload* hsm_payload,
                          brillo::SecureBlob* destination_share,
                          brillo::SecureBlob* recovery_key,
                          brillo::SecureBlob* channel_pub_key,
                          brillo::SecureBlob* channel_priv_key) const override;
  bool GenerateShares(const brillo::SecureBlob& mediator_pub_key,
                      EncryptedMediatorShare* encrypted_mediator_share,
                      brillo::SecureBlob* destination_share,
                      brillo::SecureBlob* dealer_pub_key) const override;
  bool GeneratePublisherKeys(const brillo::SecureBlob& dealer_pub_key,
                             brillo::SecureBlob* publisher_pub_key,
                             brillo::SecureBlob* publisher_dh) const override;
  bool RecoverDestination(
      const brillo::SecureBlob& publisher_pub_key,
      const brillo::SecureBlob& destination_share,
      const base::Optional<brillo::SecureBlob>& ephemeral_pub_key,
      const brillo::SecureBlob& mediated_publisher_pub_key,
      brillo::SecureBlob* destination_dh) const override;
  bool DecryptResponsePayload(
      const brillo::SecureBlob& channel_priv_key,
      const brillo::SecureBlob& epoch_pub_key,
      const brillo::SecureBlob& recovery_response_cbor,
      HsmResponsePlainText* response_plain_text) const override;

 private:
  // Encrypts mediator share and stores as `encrypted_ms` with
  // embedded ephemeral public key, AES-GCM tag and iv. Returns false if error
  // occurred.
  bool EncryptMediatorShare(const brillo::SecureBlob& mediator_pub_key,
                            const brillo::SecureBlob& mediator_share,
                            EncryptedMediatorShare* encrypted_ms,
                            BN_CTX* context) const;
  bool GenerateRecoveryKey(const crypto::ScopedEC_POINT& recovery_pub_point,
                           const crypto::ScopedEC_KEY& dealer_key_pair,
                           brillo::SecureBlob* recovery_key) const;
  // Generate ephemeral public and inverse public keys {G*x, G*-x}
  bool GenerateEphemeralKey(brillo::SecureBlob* ephemeral_pub_key,
                            brillo::SecureBlob* ephemeral_inv_pub_key) const;
  bool GenerateHsmAssociatedData(const brillo::SecureBlob& channel_pub_key,
                                 const brillo::SecureBlob& rsa_pub_key,
                                 const brillo::SecureBlob& onboarding_metadata,
                                 brillo::SecureBlob* hsm_associated_data,
                                 brillo::SecureBlob* publisher_priv_key,
                                 brillo::SecureBlob* publisher_pub_key) const;

  EllipticCurve ec_;
};

RecoveryCryptoImpl::RecoveryCryptoImpl(EllipticCurve ec) : ec_(std::move(ec)) {}

RecoveryCryptoImpl::~RecoveryCryptoImpl() = default;

bool RecoveryCryptoImpl::EncryptMediatorShare(
    const brillo::SecureBlob& mediator_pub_key,
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
  // |hkdf_salt| can be empty here because the input already has a high entropy.
  // Bruteforce attacks are not an issue here and as we generate an ephemeral
  // key as input to HKDF the output will already be non-deterministic.
  if (!GenerateEcdhHkdfSenderKey(ec_, mediator_pub_key,
                                 encrypted_ms->ephemeral_pub_key,
                                 ephemeral_priv_key, GetMediatorShareHkdfInfo(),
                                 /*hkdf_salt=*/brillo::SecureBlob(), kHkdfHash,
                                 kAesGcm256KeySize, &aes_gcm_key)) {
    LOG(ERROR) << "Failed to generate ECDH+HKDF sender keys";
    return false;
  }

  // Dispose private key.
  ephemeral_priv_key.clear();

  if (!AesGcmEncrypt(mediator_share, /*ad=*/base::nullopt, aes_gcm_key,
                     &encrypted_ms->iv, &encrypted_ms->tag,
                     &encrypted_ms->encrypted_data)) {
    LOG(ERROR) << "Failed to perform AES-GCM encryption";
    return false;
  }

  return true;
}

bool RecoveryCryptoImpl::GenerateRecoveryKey(
    const crypto::ScopedEC_POINT& recovery_pub_point,
    const crypto::ScopedEC_KEY& dealer_key_pair,
    brillo::SecureBlob* recovery_key) const {
  ScopedBN_CTX context = CreateBigNumContext();
  if (!context.get()) {
    LOG(ERROR) << "Failed to allocate BN_CTX structure";
    return false;
  }

  const BIGNUM* dealer_priv_key =
      EC_KEY_get0_private_key(dealer_key_pair.get());
  crypto::ScopedEC_POINT point_dh =
      ec_.Multiply(*recovery_pub_point, *dealer_priv_key, context.get());
  if (!point_dh) {
    LOG(ERROR) << "Failed to perform point multiplication";
    return false;
  }
  brillo::SecureBlob recovery_dh;
  if (!ec_.PointToSecureBlob(*point_dh, &recovery_dh, context.get())) {
    LOG(ERROR) << "Failed to convert EC_POINT to SecureBlob";
    return false;
  }
  // |salt| can be empty here because the input already has a high entropy.
  return Hkdf(HkdfHash::kSha256, recovery_dh, GetRecoveryKeyHkdfInfo(),
              /*salt=*/brillo::SecureBlob(), /*result_len=*/0, recovery_key);
}

bool RecoveryCryptoImpl::GenerateEphemeralKey(
    brillo::SecureBlob* ephemeral_pub_key,
    brillo::SecureBlob* ephemeral_inv_pub_key) const {
  ScopedBN_CTX context = CreateBigNumContext();
  if (!context.get()) {
    LOG(ERROR) << "Failed to allocate BN_CTX structure";
    return false;
  }

  // Generate ephemeral key pair {`ephemeral_secret`, `ephemeral_pub_key`} ({x,
  // G*x}), and the inverse public key G*-x.
  crypto::ScopedBIGNUM ephemeral_priv_key_bn =
      ec_.RandomNonZeroScalar(context.get());
  if (!ephemeral_priv_key_bn) {
    LOG(ERROR) << "Failed to generate ephemeral_priv_key_bn";
    return false;
  }
  crypto::ScopedEC_POINT ephemeral_pub_point =
      ec_.MultiplyWithGenerator(*ephemeral_priv_key_bn, context.get());
  if (!ephemeral_pub_point) {
    LOG(ERROR) << "Failed to perform MultiplyWithGenerator operation for "
                  "ephemeral_priv_key_bn";
    return false;
  }
  if (!ec_.PointToSecureBlob(*ephemeral_pub_point, ephemeral_pub_key,
                             context.get())) {
    LOG(ERROR) << "Failed to convert ephemeral_pub_point to a SecureBlob";
    return false;
  }

  if (!ec_.InvertPoint(ephemeral_pub_point.get(), context.get())) {
    LOG(ERROR) << "Failed to invert the ephemeral_pub_point";
    return false;
  }
  if (!ec_.PointToSecureBlob(*ephemeral_pub_point, ephemeral_inv_pub_key,
                             context.get())) {
    LOG(ERROR)
        << "Failed to convert inverse ephemeral_pub_point to a SecureBlob";
    return false;
  }
  return true;
}

bool RecoveryCryptoImpl::GenerateRecoveryRequest(
    const HsmPayload& hsm_payload,
    const brillo::SecureBlob& request_meta_data,
    const brillo::SecureBlob& channel_priv_key,
    const brillo::SecureBlob& channel_pub_key,
    const brillo::SecureBlob& epoch_pub_key,
    brillo::SecureBlob* recovery_request,
    brillo::SecureBlob* ephemeral_pub_key) const {
  RequestPayload request_payload;
  RecoveryRequestAssociatedData request_ad;
  request_ad.hsm_payload = hsm_payload;
  request_ad.request_meta_data = request_meta_data;
  request_ad.epoch_pub_key = epoch_pub_key;
  request_ad.request_payload_salt = CreateSecureRandomBlob(kHkdfSaltLength);
  if (!SerializeRecoveryRequestAssociatedDataToCbor(
          request_ad, &request_payload.associated_data)) {
    LOG(ERROR) << "Failed to generate associated data cbor";
    return false;
  }

  brillo::SecureBlob aes_gcm_key;
  // The static nature of `channel_pub_key` (G*s) and `epoch_pub_key` (G*r)
  // requires the need to utilize a randomized salt value in the HKDF
  // computation.
  if (!GenerateEcdhHkdfSenderKey(
          ec_, epoch_pub_key, channel_pub_key, channel_priv_key,
          GetRequestPayloadPlainTextHkdfInfo(), request_ad.request_payload_salt,
          kHkdfHash, kAesGcm256KeySize, &aes_gcm_key)) {
    LOG(ERROR) << "Failed to generate ECDH+HKDF sender keys";
    return false;
  }

  brillo::SecureBlob ephemeral_inverse_pub_key;
  if (!GenerateEphemeralKey(ephemeral_pub_key, &ephemeral_inverse_pub_key)) {
    LOG(ERROR) << "Failed to generate Ephemeral keys";
    return false;
  }

  brillo::SecureBlob plain_text_cbor;
  RecoveryRequestPlainText plain_text;
  plain_text.ephemeral_pub_inv_key = ephemeral_inverse_pub_key;
  if (!SerializeRecoveryRequestPlainTextToCbor(plain_text, &plain_text_cbor)) {
    LOG(ERROR) << "Failed to generate Recovery Request plain text cbor";
    return false;
  }

  if (!AesGcmEncrypt(plain_text_cbor, request_payload.associated_data,
                     aes_gcm_key, &request_payload.iv, &request_payload.tag,
                     &request_payload.cipher_text)) {
    LOG(ERROR) << "Failed to perform AES-GCM encryption of plain_text_cbor";
    return false;
  }

  RecoveryRequest request;
  request.request_payload = std::move(request_payload);
  if (!SerializeRecoveryRequestToCbor(request, recovery_request)) {
    LOG(ERROR) << "Failed to serialize Recovery Request to CBOR";
    return false;
  }
  return true;
}

bool RecoveryCryptoImpl::GenerateHsmPayload(
    const brillo::SecureBlob& mediator_pub_key,
    const brillo::SecureBlob& rsa_pub_key,
    const brillo::SecureBlob& onboarding_metadata,
    HsmPayload* hsm_payload,
    brillo::SecureBlob* destination_share,
    brillo::SecureBlob* recovery_key,
    brillo::SecureBlob* channel_pub_key,
    brillo::SecureBlob* channel_priv_key) const {
  ScopedBN_CTX context = CreateBigNumContext();
  if (!context.get()) {
    LOG(ERROR) << "Failed to allocate BN_CTX structure";
    return false;
  }

  // Generate dealer key pair.
  crypto::ScopedEC_KEY dealer_key_pair = ec_.GenerateKey(context.get());
  if (!dealer_key_pair) {
    LOG(ERROR) << "Failed to generate dealer key pair.";
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

  if (!BigNumToSecureBlob(*destination_share_bn, ec_.ScalarSizeInBytes(),
                          destination_share)) {
    LOG(ERROR) << "Failed to convert BIGNUM to SecureBlob";
    return false;
  }
  crypto::ScopedEC_POINT recovery_pub_point =
      ec_.MultiplyWithGenerator(*secret, context.get());
  if (!recovery_pub_point) {
    LOG(ERROR) << "Failed to perform MultiplyWithGenerator operation";
    return false;
  }

  // Generate channel key pair.
  // TODO(b/194678588): channel private key should be protected via TPM.
  crypto::ScopedEC_KEY channel_key_pair = ec_.GenerateKey(context.get());
  if (!channel_key_pair) {
    LOG(ERROR) << "Failed to generate channel key pair.";
    return false;
  }
  const EC_POINT* channel_pub_point =
      EC_KEY_get0_public_key(channel_key_pair.get());
  if (!ec_.PointToSecureBlob(*channel_pub_point, channel_pub_key,
                             context.get())) {
    LOG(ERROR) << "Failed to convert channel_pub_key to a SecureBlob";
    return false;
  }
  const BIGNUM* channel_priv_key_bn =
      EC_KEY_get0_private_key(channel_key_pair.get());
  if (!BigNumToSecureBlob(*channel_priv_key_bn, ec_.ScalarSizeInBytes(),
                          channel_priv_key)) {
    LOG(ERROR) << "Failed to convert channel_priv_key_bn to a SecureBlob";
    return false;
  }

  // Construct associated data for HSM payload: AD = CBOR({publisher_pub_key,
  // channel_pub_key, rsa_pub_key, onboarding_metadata}).
  brillo::SecureBlob publisher_priv_key;
  brillo::SecureBlob publisher_pub_key;
  if (!GenerateHsmAssociatedData(*channel_pub_key, rsa_pub_key,
                                 onboarding_metadata,
                                 &hsm_payload->associated_data,
                                 &publisher_priv_key, &publisher_pub_key)) {
    LOG(ERROR) << "Failed to generate associated data cbor";
    return false;
  }

  // Construct plain text for HSM payload PT = CBOR({dealer_pub_key,
  // mediator_share, kav}).
  const EC_POINT* dealer_pub_point =
      EC_KEY_get0_public_key(dealer_key_pair.get());
  brillo::SecureBlob dealer_pub_key;
  if (!ec_.PointToSecureBlob(*dealer_pub_point, &dealer_pub_key,
                             context.get())) {
    LOG(ERROR) << "Failed to convert dealer_pub_key to a SecureBlob";
    return false;
  }
  brillo::SecureBlob mediator_share;
  if (!BigNumToSecureBlob(*mediator_share_bn, ec_.ScalarSizeInBytes(),
                          &mediator_share)) {
    LOG(ERROR) << "Failed to convert mediator_share to a SecureBlob";
    return false;
  }
  // TODO(mslus): in the initial version kav will be empty (as it should for
  // TPM 2.0). In the next iteration we will generate kav if a non-empty value
  // of `rsa_pub_key` is provided.
  brillo::SecureBlob plain_text_cbor;
  HsmPlainText hsm_plain_text;
  hsm_plain_text.mediator_share = mediator_share;
  hsm_plain_text.dealer_pub_key = dealer_pub_key;
  if (!SerializeHsmPlainTextToCbor(hsm_plain_text, &plain_text_cbor)) {
    LOG(ERROR) << "Failed to generate HSM plain text cbor";
    return false;
  }

  brillo::SecureBlob aes_gcm_key;
  // |hkdf_salt| can be empty here because the input already has a high entropy.
  // Bruteforce attacks are not an issue here and as we generate an ephemeral
  // key as input to HKDF the output will already be non-deterministic.
  if (!GenerateEcdhHkdfSenderKey(ec_, mediator_pub_key, publisher_pub_key,
                                 publisher_priv_key, GetMediatorShareHkdfInfo(),
                                 /*hkdf_salt=*/brillo::SecureBlob(), kHkdfHash,
                                 kAesGcm256KeySize, &aes_gcm_key)) {
    LOG(ERROR) << "Failed to generate ECDH+HKDF sender keys";
    return false;
  }

  if (!AesGcmEncrypt(plain_text_cbor, hsm_payload->associated_data, aes_gcm_key,
                     &hsm_payload->iv, &hsm_payload->tag,
                     &hsm_payload->cipher_text)) {
    LOG(ERROR) << "Failed to perform AES-GCM encryption";
    return false;
  }

  // Cleanup: all intermediate secrets must be securely disposed at the end of
  // HSM payload generation.
  aes_gcm_key.clear();
  plain_text_cbor.clear();
  mediator_share.clear();
  dealer_pub_key.clear();
  publisher_pub_key.clear();
  publisher_priv_key.clear();

  GenerateRecoveryKey(recovery_pub_point, dealer_key_pair, recovery_key);
  return true;
}

bool RecoveryCryptoImpl::GenerateShares(
    const brillo::SecureBlob& mediator_pub_key,
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
  if (!EncryptMediatorShare(mediator_pub_key, mediator_share,
                            encrypted_mediator_share, context.get())) {
    LOG(ERROR) << "Failed to encrypt mediator share";
    return false;
  }
  return true;
}

bool RecoveryCryptoImpl::GeneratePublisherKeys(
    const brillo::SecureBlob& dealer_pub_key,
    brillo::SecureBlob* publisher_pub_key,
    brillo::SecureBlob* publisher_recovery_key) const {
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
  brillo::SecureBlob publisher_dh;
  if (!ec_.PointToSecureBlob(*point_dh, &publisher_dh, context.get())) {
    LOG(ERROR) << "Failed to convert EC_POINT to SecureBlob";
    return false;
  }
  // |hkdf_salt| can be empty here because the input already has a high entropy.
  if (!Hkdf(HkdfHash::kSha256, publisher_dh, GetRecoveryKeyHkdfInfo(),
            /*salt=*/brillo::SecureBlob(),
            /*result_len=*/0, publisher_recovery_key)) {
    return false;
  }
  return true;
}

bool RecoveryCryptoImpl::RecoverDestination(
    const brillo::SecureBlob& publisher_pub_key,
    const brillo::SecureBlob& destination_share,
    const base::Optional<brillo::SecureBlob>& ephemeral_pub_key,
    const brillo::SecureBlob& mediated_publisher_pub_key,
    brillo::SecureBlob* destination_recovery_key) const {
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
  crypto::ScopedEC_POINT mediator_dh =
      ec_.SecureBlobToPoint(mediated_publisher_pub_key, context.get());
  if (!mediator_dh) {
    LOG(ERROR) << "Failed to convert SecureBlob to EC_POINT";
    return false;
  }
  // TODO(b/194884283): Make ephemeral_pub_key non-optional after old protocol
  // version is removed.
  if (ephemeral_pub_key.has_value()) {
    // Performs addition of mediator_dh and ephemeral_pub_key.
    crypto::ScopedEC_POINT ephemeral_pub_point =
        ec_.SecureBlobToPoint(ephemeral_pub_key.value(), context.get());
    if (!ephemeral_pub_point) {
      LOG(ERROR) << "Failed to convert SecureBlob to EC_POINT";
      return false;
    }
    mediator_dh = ec_.Add(*mediator_dh, *ephemeral_pub_point, context.get());
    if (!mediator_dh) {
      LOG(ERROR) << "Failed to add mediator_dh and ephemeral_pub_point";
      return false;
    }
  }
  // Performs scalar multiplication of publisher_pub_key and destination_share.
  crypto::ScopedEC_POINT point_dh =
      ec_.Multiply(*publisher_pub_point, *destination_share_bn, context.get());
  if (!point_dh) {
    LOG(ERROR) << "Failed to perform scalar multiplication";
    return false;
  }
  crypto::ScopedEC_POINT point_dest =
      ec_.Add(*point_dh, *mediator_dh, context.get());
  if (!point_dest) {
    LOG(ERROR) << "Failed to perform point addition";
    return false;
  }
  brillo::SecureBlob destination_dh;
  if (!ec_.PointToSecureBlob(*point_dest, &destination_dh, context.get())) {
    LOG(ERROR) << "Failed to convert EC_POINT to SecureBlob";
    return false;
  }
  // |hkdf_salt| can be empty here because the input already has a high entropy.
  if (!Hkdf(HkdfHash::kSha256, destination_dh, GetRecoveryKeyHkdfInfo(),
            /*salt=*/brillo::SecureBlob(), /*result_len=*/0,
            destination_recovery_key)) {
    return false;
  }
  return true;
}

bool RecoveryCryptoImpl::DecryptResponsePayload(
    const brillo::SecureBlob& channel_priv_key,
    const brillo::SecureBlob& epoch_pub_key,
    const brillo::SecureBlob& recovery_response_cbor,
    HsmResponsePlainText* response_plain_text) const {
  RecoveryResponse recovery_response;
  if (!DeserializeRecoveryResponseFromCbor(recovery_response_cbor,
                                           &recovery_response)) {
    LOG(ERROR) << "Unable to deserialize Recovery Response from CBOR";
    return false;
  }

  HsmResponseAssociatedData response_ad;
  if (!DeserializeHsmResponseAssociatedDataFromCbor(
          recovery_response.response_payload.associated_data, &response_ad)) {
    LOG(ERROR) << "Unable to deserialize Response payload associated data";
    return false;
  }
  brillo::SecureBlob aes_gcm_key;
  if (!GenerateEcdhHkdfRecipientKey(ec_, channel_priv_key, epoch_pub_key,
                                    GetResponsePayloadPlainTextHkdfInfo(),
                                    response_ad.response_payload_salt,
                                    RecoveryCrypto::kHkdfHash,
                                    kAesGcm256KeySize, &aes_gcm_key)) {
    LOG(ERROR) << "Failed to generate ECDH+HKDF recipient key for mediator "
                  "share decryption";
    return false;
  }
  brillo::SecureBlob response_plain_text_cbor;
  if (!AesGcmDecrypt(recovery_response.response_payload.cipher_text,
                     recovery_response.response_payload.associated_data,
                     recovery_response.response_payload.tag, aes_gcm_key,
                     recovery_response.response_payload.iv,
                     &response_plain_text_cbor)) {
    LOG(ERROR) << "Failed to perform AES-GCM decryption";
    return false;
  }
  if (!DeserializeHsmResponsePlainTextFromCbor(response_plain_text_cbor,
                                               response_plain_text)) {
    LOG(ERROR) << "Failed to deserialize Response plain text";
    return false;
  }
  return true;
}

bool RecoveryCryptoImpl::GenerateHsmAssociatedData(
    const brillo::SecureBlob& channel_pub_key,
    const brillo::SecureBlob& rsa_pub_key,
    const brillo::SecureBlob& onboarding_metadata,
    brillo::SecureBlob* hsm_associated_data,
    brillo::SecureBlob* publisher_priv_key,
    brillo::SecureBlob* publisher_pub_key) const {
  ScopedBN_CTX context = CreateBigNumContext();
  if (!context.get()) {
    LOG(ERROR) << "Failed to allocate BN_CTX structure";
    return false;
  }

  // Generate publisher key pair.
  crypto::ScopedEC_KEY publisher_key_pair = ec_.GenerateKey(context.get());
  if (!publisher_key_pair) {
    LOG(ERROR) << "Failed to generate publisher key pair.";
    return false;
  }

  // Construct associated data for HSM payload: AD = CBOR({publisher_pub_key,
  // channel_pub_key, rsa_pub_key, onboarding_metadata}).
  const EC_POINT* publisher_pub_point =
      EC_KEY_get0_public_key(publisher_key_pair.get());
  if (!ec_.PointToSecureBlob(*publisher_pub_point, publisher_pub_key,
                             context.get())) {
    LOG(ERROR) << "Failed to convert publisher_pub_key to a SecureBlob";
    return false;
  }
  const BIGNUM* publisher_priv_secret =
      EC_KEY_get0_private_key(publisher_key_pair.get());
  if (!BigNumToSecureBlob(*publisher_priv_secret, ec_.ScalarSizeInBytes(),
                          publisher_priv_key)) {
    LOG(ERROR) << "Failed to convert publisher_priv_key to a SecureBlob";
    return false;
  }
  HsmAssociatedData hsm_ad;
  hsm_ad.publisher_pub_key = *publisher_pub_key;
  hsm_ad.channel_pub_key = channel_pub_key;
  hsm_ad.rsa_public_key = rsa_pub_key;
  hsm_ad.onboarding_meta_data = onboarding_metadata;
  if (!SerializeHsmAssociatedDataToCbor(hsm_ad, hsm_associated_data)) {
    LOG(ERROR) << "Failed to generate associated data cbor";
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

}  // namespace cryptorecovery
}  // namespace cryptohome