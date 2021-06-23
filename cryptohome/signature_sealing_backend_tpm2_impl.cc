// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cryptohome/signature_sealing_backend_tpm2_impl.h"

#include <stdint.h>
#include <cstring>
#include <string>
#include <utility>

#include <base/check.h>
#include <base/check_op.h>
#include <base/logging.h>
#include <base/memory/ptr_util.h>
#include <base/notreached.h>
#include <base/numerics/safe_conversions.h>
#include <base/optional.h>
#include <base/threading/thread_checker.h>
#include <brillo/secure_blob.h>
#include <google/protobuf/repeated_field.h>
#include <libhwsec/error/tpm2_error.h>
#include <trunks/error_codes.h>
#include <trunks/policy_session.h>
#include <trunks/tpm_generated.h>
#include <trunks/trunks_factory_impl.h>
#include <trunks/hmac_session.h>
#include <trunks/tpm_utility.h>
#include <trunks/authorization_delegate.h>

#include "cryptohome/key.pb.h"
#include "cryptohome/signature_sealed_data.pb.h"
#include "cryptohome/tpm2_impl.h"

using brillo::Blob;
using brillo::BlobFromString;
using brillo::BlobToString;
using brillo::CombineBlobs;
using brillo::SecureBlob;
using hwsec::error::TPM2Error;
using hwsec::error::TPMError;
using hwsec::error::TPMErrorBase;
using hwsec::error::TPMRetryAction;
using hwsec_foundation::error::CreateError;
using hwsec_foundation::error::CreateErrorWrap;
using trunks::GetErrorString;
using trunks::TPM_ALG_ID;
using trunks::TPM_ALG_NULL;
using trunks::TPM_RC;
using trunks::TPM_RC_SUCCESS;

namespace cryptohome {

namespace {

// Size, in bytes, of the secret value that is generated by
// SignatureSealingBackendTpm2Impl::CreateSealedSecret().
constexpr int kSecretSizeBytes = 32;

class UnsealingSessionTpm2Impl final
    : public SignatureSealingBackend::UnsealingSession {
 public:
  UnsealingSessionTpm2Impl(
      Tpm2Impl* tpm,
      Tpm2Impl::TrunksClientContext* trunks,
      const Blob& srk_wrapped_secret,
      const Blob& public_key_spki_der,
      ChallengeSignatureAlgorithm algorithm,
      TPM_ALG_ID scheme,
      TPM_ALG_ID hash_alg,
      std::unique_ptr<trunks::PolicySession> policy_session,
      const Blob& policy_session_tpm_nonce);
  UnsealingSessionTpm2Impl(const UnsealingSessionTpm2Impl&) = delete;
  UnsealingSessionTpm2Impl& operator=(const UnsealingSessionTpm2Impl&) = delete;

  ~UnsealingSessionTpm2Impl() override;

  // UnsealingSession:
  ChallengeSignatureAlgorithm GetChallengeAlgorithm() override;
  Blob GetChallengeValue() override;
  TPMErrorBase Unseal(const Blob& signed_challenge_value,
                      SecureBlob* unsealed_value) override;

 private:
  // Unowned.
  Tpm2Impl* const tpm_;
  // Unowned.
  Tpm2Impl::TrunksClientContext* const trunks_;
  const Blob srk_wrapped_secret_;
  const Blob public_key_spki_der_;
  const ChallengeSignatureAlgorithm algorithm_;
  const TPM_ALG_ID scheme_;
  const TPM_ALG_ID hash_alg_;
  const std::unique_ptr<trunks::PolicySession> policy_session_;
  const Blob policy_session_tpm_nonce_;
  base::ThreadChecker thread_checker_;
};

// Obtains the TPM 2.0 signature scheme and hashing algorithms that correspond
// to the provided challenge signature algorithm.
bool GetAlgIdsByAlgorithm(ChallengeSignatureAlgorithm algorithm,
                          TPM_ALG_ID* scheme,
                          TPM_ALG_ID* hash_alg) {
  switch (algorithm) {
    case CHALLENGE_RSASSA_PKCS1_V1_5_SHA1:
      *scheme = trunks::TPM_ALG_RSASSA;
      *hash_alg = trunks::TPM_ALG_SHA1;
      return true;
    case CHALLENGE_RSASSA_PKCS1_V1_5_SHA256:
      *scheme = trunks::TPM_ALG_RSASSA;
      *hash_alg = trunks::TPM_ALG_SHA256;
      return true;
    case CHALLENGE_RSASSA_PKCS1_V1_5_SHA384:
      *scheme = trunks::TPM_ALG_RSASSA;
      *hash_alg = trunks::TPM_ALG_SHA384;
      return true;
    case CHALLENGE_RSASSA_PKCS1_V1_5_SHA512:
      *scheme = trunks::TPM_ALG_RSASSA;
      *hash_alg = trunks::TPM_ALG_SHA512;
      return true;
  }
  NOTREACHED();
  return false;
}

// Given the list of alternative sets of PCR restrictions, returns the one that
// is currently satisfied. Returns null if none is satisfied.
const SignatureSealedData_Tpm2PcrRestriction* GetSatisfiedPcrRestriction(
    const google::protobuf::RepeatedPtrField<
        SignatureSealedData_Tpm2PcrRestriction>& pcr_restrictions,
    Tpm* tpm) {
  std::map<uint32_t, Blob> current_pcr_values;
  for (const auto& pcr_restriction_proto : pcr_restrictions) {
    bool is_satisfied = true;
    for (const auto& pcr_value_proto : pcr_restriction_proto.pcr_values()) {
      const uint32_t pcr_index = pcr_value_proto.pcr_index();
      if (pcr_index >= IMPLEMENTATION_PCR) {
        LOG(WARNING) << "Invalid PCR index " << pcr_index;
        is_satisfied = false;
        break;
      }
      if (!current_pcr_values.count(pcr_index)) {
        Blob pcr_value;
        if (!tpm->ReadPCR(pcr_index, &pcr_value)) {
          is_satisfied = false;
          break;
        }
        current_pcr_values.emplace(pcr_index, pcr_value);
      }
      if (current_pcr_values[pcr_index] !=
          BlobFromString(pcr_value_proto.pcr_value())) {
        is_satisfied = false;
        break;
      }
    }
    if (is_satisfied)
      return &pcr_restriction_proto;
  }
  return nullptr;
}

UnsealingSessionTpm2Impl::UnsealingSessionTpm2Impl(
    Tpm2Impl* tpm,
    Tpm2Impl::TrunksClientContext* trunks,
    const Blob& srk_wrapped_secret,
    const Blob& public_key_spki_der,
    ChallengeSignatureAlgorithm algorithm,
    TPM_ALG_ID scheme,
    TPM_ALG_ID hash_alg,
    std::unique_ptr<trunks::PolicySession> policy_session,
    const Blob& policy_session_tpm_nonce)
    : tpm_(tpm),
      trunks_(trunks),
      srk_wrapped_secret_(srk_wrapped_secret),
      public_key_spki_der_(public_key_spki_der),
      algorithm_(algorithm),
      scheme_(scheme),
      hash_alg_(hash_alg),
      policy_session_(std::move(policy_session)),
      policy_session_tpm_nonce_(policy_session_tpm_nonce) {}

UnsealingSessionTpm2Impl::~UnsealingSessionTpm2Impl() {
  DCHECK(thread_checker_.CalledOnValidThread());
}

ChallengeSignatureAlgorithm UnsealingSessionTpm2Impl::GetChallengeAlgorithm() {
  DCHECK(thread_checker_.CalledOnValidThread());
  return algorithm_;
}

Blob UnsealingSessionTpm2Impl::GetChallengeValue() {
  DCHECK(thread_checker_.CalledOnValidThread());
  const Blob expiration_blob(4);  // zero expiration (4-byte integer)
  return CombineBlobs({policy_session_tpm_nonce_, expiration_blob});
}

TPMErrorBase UnsealingSessionTpm2Impl::Unseal(
    const Blob& signed_challenge_value, SecureBlob* unsealed_value) {
  DCHECK(thread_checker_.CalledOnValidThread());
  // Start a TPM authorization session.
  std::unique_ptr<trunks::HmacSession> session =
      trunks_->factory->GetHmacSession();
  if (auto err = CreateError<TPM2Error>(
          trunks_->tpm_utility->StartSession(session.get()))) {
    return CreateErrorWrap<TPMError>(std::move(err),
                                     "Error starting hmac session");
  }
  // Load the protection public key onto the TPM.
  ScopedKeyHandle key_handle;
  if (!tpm_->LoadPublicKeyFromSpki(
          public_key_spki_der_, AsymmetricKeyUsage::kSignKey, scheme_,
          hash_alg_, session->GetDelegate(), &key_handle)) {
    return CreateError<TPMError>("Error loading protection key",
                                 TPMRetryAction::kNoRetry);
  }
  std::string key_name;
  if (auto err = CreateError<TPM2Error>(
          trunks_->tpm_utility->GetKeyName(key_handle.value(), &key_name))) {
    return CreateErrorWrap<TPMError>(std::move(err), "Failed to get key name");
  }
  // Update the policy with the signature.
  trunks::TPMT_SIGNATURE signature;
  memset(&signature, 0, sizeof(trunks::TPMT_SIGNATURE));
  signature.sig_alg = scheme_;
  signature.signature.rsassa.hash = hash_alg_;
  signature.signature.rsassa.sig =
      trunks::Make_TPM2B_PUBLIC_KEY_RSA(BlobToString(signed_challenge_value));
  if (auto err = CreateError<TPM2Error>(policy_session_->PolicySigned(
          key_handle.value(), key_name, BlobToString(policy_session_tpm_nonce_),
          std::string() /* cp_hash */, std::string() /* policy_ref */,
          0 /* expiration */, signature, session->GetDelegate()))) {
    return CreateErrorWrap<TPMError>(
        std::move(err),
        "Error restricting policy to signature with the public key");
  }
  // Obtain the resulting policy digest.
  std::string policy_digest;
  if (auto err =
          CreateError<TPM2Error>(policy_session_->GetDigest(&policy_digest))) {
    return CreateErrorWrap<TPMError>(std::move(err),
                                     "Error getting policy digest");
  }
  // Unseal the secret value.
  std::string unsealed_value_string;
  if (auto err = CreateError<TPM2Error>(trunks_->tpm_utility->UnsealData(
          BlobToString(srk_wrapped_secret_), policy_session_->GetDelegate(),
          &unsealed_value_string))) {
    return CreateErrorWrap<TPMError>(std::move(err), "Error unsealing object");
  }
  *unsealed_value = SecureBlob(unsealed_value_string);
  return nullptr;
}

}  // namespace

SignatureSealingBackendTpm2Impl::SignatureSealingBackendTpm2Impl(Tpm2Impl* tpm)
    : tpm_(tpm) {}

SignatureSealingBackendTpm2Impl::~SignatureSealingBackendTpm2Impl() = default;

TPMErrorBase SignatureSealingBackendTpm2Impl::CreateSealedSecret(
    const Blob& public_key_spki_der,
    const std::vector<ChallengeSignatureAlgorithm>& key_algorithms,
    const std::vector<std::map<uint32_t, brillo::Blob>>& pcr_restrictions,
    const Blob& /* delegate_blob */,
    const Blob& /* delegate_secret */,
    brillo::SecureBlob* secret_value,
    SignatureSealedData* sealed_secret_data) {
  // Choose the algorithm. Respect the input's algorithm prioritization, with
  // the exception of considering SHA-1 as the least preferred option.
  TPM_ALG_ID scheme = TPM_ALG_NULL;
  TPM_ALG_ID hash_alg = TPM_ALG_NULL;
  for (auto algorithm : key_algorithms) {
    TPM_ALG_ID current_scheme = TPM_ALG_NULL;
    TPM_ALG_ID current_hash_alg = TPM_ALG_NULL;
    if (GetAlgIdsByAlgorithm(algorithm, &current_scheme, &current_hash_alg)) {
      scheme = current_scheme;
      hash_alg = current_hash_alg;
      if (hash_alg != trunks::TPM_ALG_SHA1)
        break;
    }
  }
  if (scheme == TPM_ALG_NULL) {
    return CreateError<TPMError>("Error choosing the signature algorithm",
                                 TPMRetryAction::kNoRetry);
  }
  // Start a TPM authorization session.
  Tpm2Impl::TrunksClientContext* trunks = nullptr;
  if (!tpm_->GetTrunksContext(&trunks)) {
    return CreateError<TPMError>("Failed to get trunks context",
                                 TPMRetryAction::kNoRetry);
  }
  std::unique_ptr<trunks::HmacSession> session =
      trunks->factory->GetHmacSession();
  if (auto err = CreateError<TPM2Error>(
          trunks->tpm_utility->StartSession(session.get()))) {
    return CreateErrorWrap<TPMError>(std::move(err),
                                     "Error starting hmac session");
  }
  // Load the protection public key onto the TPM.
  ScopedKeyHandle key_handle;
  if (!tpm_->LoadPublicKeyFromSpki(
          public_key_spki_der, AsymmetricKeyUsage::kSignKey, scheme, hash_alg,
          session->GetDelegate(), &key_handle)) {
    return CreateError<TPMError>("Error loading protection key",
                                 TPMRetryAction::kNoRetry);
  }
  std::string key_name;
  if (auto err = CreateError<TPM2Error>(
          trunks->tpm_utility->GetKeyName(key_handle.value(), &key_name))) {
    return CreateErrorWrap<TPMError>(std::move(err), "Failed to get key name");
  }
  // Start a trial policy session for sealing the secret value.
  std::unique_ptr<trunks::PolicySession> policy_session =
      trunks->factory->GetTrialSession();
  if (auto err = CreateError<TPM2Error>(
          policy_session->StartUnboundSession(true, false))) {
    return CreateErrorWrap<TPMError>(std::move(err),
                                     "Error starting a trial session");
  }
  // Calculate policy digests for each of the sets of PCR restrictions
  // separately. Rewind each time the policy session back to the initial state,
  // except when we're in the degenerate case of only one set of PCRs (so that
  // no PolicyOR command should be used, and we should just proceed with the
  // PolicyPCR result).
  std::vector<std::string> pcr_policy_digests;
  for (const auto& pcr_values : pcr_restrictions) {
    DCHECK(!pcr_values.empty());
    // Run PolicyPCR against the current PCR set.
    std::map<uint32_t, std::string> pcr_values_strings;
    for (const auto& pcr_index_and_value : pcr_values) {
      pcr_values_strings[pcr_index_and_value.first] =
          BlobToString(pcr_index_and_value.second);
    }
    if (auto err = CreateError<TPM2Error>(
            policy_session->PolicyPCR(pcr_values_strings))) {
      return CreateErrorWrap<TPMError>(std::move(err),
                                       "Error restricting policy to PCRs");
    }
    // Remember the policy digest for the current PCR set.
    std::string pcr_policy_digest;
    if (auto err = CreateError<TPM2Error>(
            policy_session->GetDigest(&pcr_policy_digest))) {
      return CreateErrorWrap<TPMError>(std::move(err),
                                       "Error getting policy digest");
    }
    pcr_policy_digests.push_back(pcr_policy_digest);
    // Restart the policy session when necessary.
    if (pcr_restrictions.size() > 1) {
      if (auto err = CreateError<TPM2Error>(policy_session->PolicyRestart())) {
        return CreateErrorWrap<TPMError>(std::move(err),
                                         "Error restarting the policy session");
      }
    }
  }
  // If necessary, apply PolicyOR for restricting to the disjunction of the
  // specified sets of PCR restrictions.
  if (pcr_restrictions.size() > 1) {
    if (auto err = CreateError<TPM2Error>(
            policy_session->PolicyOR(pcr_policy_digests))) {
      return CreateErrorWrap<TPMError>(
          std::move(err),
          "Error restricting policy to logical disjunction of PCRs");
    }
  }
  // Update the policy with an empty signature that refers to the public key.
  trunks::TPMT_SIGNATURE signature;
  memset(&signature, 0, sizeof(trunks::TPMT_SIGNATURE));
  signature.sig_alg = scheme;
  signature.signature.rsassa.hash = hash_alg;
  signature.signature.rsassa.sig =
      trunks::Make_TPM2B_PUBLIC_KEY_RSA(std::string());
  if (auto err = CreateError<TPM2Error>(policy_session->PolicySigned(
          key_handle.value(), key_name, std::string() /* nonce */,
          std::string() /* cp_hash */, std::string() /* policy_ref */,
          0 /* expiration */, signature, session->GetDelegate()))) {
    return CreateErrorWrap<TPMError>(
        std::move(err),
        "Error restricting policy to signature with the public key");
  }
  // Obtain the resulting policy digest.
  std::string policy_digest;
  if (auto err =
          CreateError<TPM2Error>(policy_session->GetDigest(&policy_digest))) {
    return CreateErrorWrap<TPMError>(std::move(err),
                                     "Error getting policy digest");
  }
  if (policy_digest.size() != SHA256_DIGEST_SIZE) {
    return CreateError<TPMError>("Unexpected policy digest size",
                                 TPMRetryAction::kNoRetry);
  }
  // Generate the secret value randomly.
  if (auto err =
          tpm_->GetRandomDataSecureBlob(kSecretSizeBytes, secret_value)) {
    return CreateErrorWrap<TPMError>(std::move(err),
                                     "Error generating random secret");
  }
  DCHECK_EQ(secret_value->size(), kSecretSizeBytes);
  // Seal the secret value.
  std::string sealed_value;
  if (auto err = CreateError<TPM2Error>(trunks->tpm_utility->SealData(
          secret_value->to_string(), policy_digest, "", session->GetDelegate(),
          &sealed_value))) {
    return CreateErrorWrap<TPMError>(std::move(err),
                                     "Error sealing secret data");
  }
  // Fill the resulting proto with data required for unsealing.
  sealed_secret_data->Clear();
  SignatureSealedData_Tpm2PolicySignedData* const data_proto =
      sealed_secret_data->mutable_tpm2_policy_signed_data();
  data_proto->set_public_key_spki_der(BlobToString(public_key_spki_der));
  data_proto->set_srk_wrapped_secret(sealed_value);
  data_proto->set_scheme(scheme);
  data_proto->set_hash_alg(hash_alg);
  for (size_t restriction_index = 0;
       restriction_index < pcr_restrictions.size(); ++restriction_index) {
    const auto& pcr_values = pcr_restrictions[restriction_index];
    SignatureSealedData_Tpm2PcrRestriction* const pcr_restriction_proto =
        data_proto->add_pcr_restrictions();
    for (const auto& pcr_index_and_value : pcr_values) {
      SignatureSealedData_PcrValue* const pcr_value_proto =
          pcr_restriction_proto->add_pcr_values();
      pcr_value_proto->set_pcr_index(pcr_index_and_value.first);
      pcr_value_proto->set_pcr_value(BlobToString(pcr_index_and_value.second));
    }
    pcr_restriction_proto->set_policy_digest(
        pcr_policy_digests[restriction_index]);
  }
  return nullptr;
}

TPMErrorBase SignatureSealingBackendTpm2Impl::CreateUnsealingSession(
    const SignatureSealedData& sealed_secret_data,
    const Blob& public_key_spki_der,
    const std::vector<ChallengeSignatureAlgorithm>& key_algorithms,
    const Blob& /* delegate_blob */,
    const Blob& /* delegate_secret */,
    std::unique_ptr<SignatureSealingBackend::UnsealingSession>*
        unsealing_session) {
  // Validate the parameters.
  if (!sealed_secret_data.has_tpm2_policy_signed_data()) {
    return CreateError<TPMError>(
        "Sealed data is empty or uses unexpected method",
        TPMRetryAction::kNoRetry);
  }
  const SignatureSealedData_Tpm2PolicySignedData& data_proto =
      sealed_secret_data.tpm2_policy_signed_data();
  if (data_proto.public_key_spki_der() != BlobToString(public_key_spki_der)) {
    return CreateError<TPMError>("Wrong subject public key info",
                                 TPMRetryAction::kNoRetry);
  }
  if (!base::IsValueInRangeForNumericType<TPM_ALG_ID>(data_proto.scheme())) {
    return CreateError<TPMError>("Error parsing signature scheme",
                                 TPMRetryAction::kNoRetry);
  }
  const TPM_ALG_ID scheme = static_cast<TPM_ALG_ID>(data_proto.scheme());
  if (!base::IsValueInRangeForNumericType<TPM_ALG_ID>(data_proto.hash_alg())) {
    return CreateError<TPMError>("Error parsing signature hash algorithm",
                                 TPMRetryAction::kNoRetry);
  }
  const TPM_ALG_ID hash_alg = static_cast<TPM_ALG_ID>(data_proto.hash_alg());
  base::Optional<ChallengeSignatureAlgorithm> chosen_algorithm;
  for (auto algorithm : key_algorithms) {
    TPM_ALG_ID current_scheme = TPM_ALG_NULL;
    TPM_ALG_ID current_hash_alg = TPM_ALG_NULL;
    if (GetAlgIdsByAlgorithm(algorithm, &current_scheme, &current_hash_alg) &&
        current_scheme == scheme && current_hash_alg == hash_alg) {
      chosen_algorithm = algorithm;
      break;
    }
  }
  if (!chosen_algorithm) {
    return CreateError<TPMError>("Key doesn't support required algorithm",
                                 TPMRetryAction::kNoRetry);
  }
  // Obtain the trunks context to be used for the whole unsealing session.
  Tpm2Impl::TrunksClientContext* trunks = nullptr;
  if (!tpm_->GetTrunksContext(&trunks)) {
    return CreateError<TPMError>("Failed to get trunks context",
                                 TPMRetryAction::kNoRetry);
  }
  // Start a policy session that will be used for obtaining the TPM nonce and
  // unsealing the secret value.
  std::unique_ptr<trunks::PolicySession> policy_session =
      trunks->factory->GetPolicySession();
  if (auto err = CreateError<TPM2Error>(
          policy_session->StartUnboundSession(true, false))) {
    return CreateErrorWrap<TPMError>(std::move(err),
                                     "Error starting a policy session");
  }
  // If PCR restrictions were applied, update the policy correspondingly.
  if (data_proto.pcr_restrictions_size()) {
    // Determine the satisfied set of PCR restrictions and update the policy
    // with it.
    const SignatureSealedData_Tpm2PcrRestriction* const
        satisfied_pcr_restriction_proto =
            GetSatisfiedPcrRestriction(data_proto.pcr_restrictions(), tpm_);
    if (!satisfied_pcr_restriction_proto) {
      return CreateError<TPMError>("None of PCR restrictions is satisfied",
                                   TPMRetryAction::kNoRetry);
    }
    std::map<uint32_t, std::string> pcrs_to_apply;
    for (const auto& pcr_value_proto :
         satisfied_pcr_restriction_proto->pcr_values()) {
      pcrs_to_apply.emplace(pcr_value_proto.pcr_index(), std::string());
    }

    if (auto err =
            CreateError<TPM2Error>(policy_session->PolicyPCR(pcrs_to_apply))) {
      return CreateErrorWrap<TPMError>(std::move(err),
                                       "Error restricting policy to PCRs");
    }
    // If more than one set of PCR restrictions was originally specified, update
    // the policy with the disjunction of their policy digests.
    if (data_proto.pcr_restrictions_size() > 1) {
      std::vector<std::string> pcr_policy_digests;
      for (const auto& pcr_restriction_proto : data_proto.pcr_restrictions()) {
        if (pcr_restriction_proto.policy_digest().size() !=
            SHA256_DIGEST_SIZE) {
          return CreateError<TPMError>("Invalid policy digest size",
                                       TPMRetryAction::kNoRetry);
        }
        pcr_policy_digests.push_back(pcr_restriction_proto.policy_digest());
      }
      if (auto err = CreateError<TPM2Error>(
              policy_session->PolicyOR(pcr_policy_digests))) {
        return CreateErrorWrap<TPMError>(
            std::move(err),
            "Error restricting policy to logical disjunction of PCRs");
      }
    }
  }
  // Obtain the TPM nonce.
  std::string tpm_nonce;
  if (!policy_session->GetDelegate()->GetTpmNonce(&tpm_nonce)) {
    return CreateError<TPMError>("Error obtaining TPM nonce",
                                 TPMRetryAction::kNoRetry);
  }
  // Create the unsealing session that will keep the required state.
  *unsealing_session = std::make_unique<UnsealingSessionTpm2Impl>(
      tpm_, trunks, BlobFromString(data_proto.srk_wrapped_secret()),
      public_key_spki_der, *chosen_algorithm, scheme, hash_alg,
      std::move(policy_session), BlobFromString(tpm_nonce));
  return nullptr;
}

}  // namespace cryptohome
