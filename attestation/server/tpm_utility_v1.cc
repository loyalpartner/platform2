// Copyright 2015 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "attestation/server/tpm_utility_v1.h"

#include <base/files/file_path.h>
#include <base/files/file_util.h>
#include <base/logging.h>
#include <base/memory/scoped_ptr.h>
#include <base/stl_util.h>
#include <crypto/scoped_openssl_types.h>
#include <openssl/rsa.h>
#include <trousers/scoped_tss_type.h>
#include <trousers/trousers.h>
#include <trousers/tss.h>

#define TPM_LOG(severity, result) \
    LOG(severity) << "TPM error 0x" << std::hex << result \
                  << " (" << Trspi_Error_String(result) << "): "

using trousers::ScopedTssContext;
using trousers::ScopedTssKey;
using trousers::ScopedTssMemory;

namespace {

typedef scoped_ptr<BYTE, base::FreeDeleter> ScopedByteArray;

const char* kTpmEnabledFile = "/sys/class/misc/tpm0/device/enabled";
const char* kTpmOwnedFile = "/sys/class/misc/tpm0/device/owned";
const unsigned int kWellKnownExponent = 65537;

std::string GetFirstByte(const char* file_name) {
  std::string content;
  base::ReadFileToString(base::FilePath(file_name), &content);
  if (content.size() > 1) {
    content.resize(1);
  }
  return content;
}

}  // namespace

namespace attestation {

TpmUtilityV1::~TpmUtilityV1() {}

bool TpmUtilityV1::Initialize() {
  if (!ConnectContext(&context_handle_, &tpm_handle_)) {
    LOG(ERROR) << __func__ << ": Failed to connect to the TPM.";
    return false;
  }
  srk_handle_.reset(context_handle_, 0);
  if (!LoadSrk(context_handle_, &srk_handle_)) {
    LOG(ERROR) << __func__ << ": Failed to load SRK.";
    return false;
  }
  // In order to wrap a key with the SRK we need access to the SRK public key
  // and we need to get it manually. Once it's in the key object, we don't need
  // to do this again.
  UINT32 length = 0;
  ScopedTssMemory buffer(context_handle_);
  TSS_RESULT result;
  result = Tspi_Key_GetPubKey(srk_handle_, &length, buffer.ptr());
  if (result != TSS_SUCCESS) {
    TPM_LOG(INFO, result) << __func__ << ": Failed to read SRK public key.";
    return false;
  }
  return true;
}

bool TpmUtilityV1::IsTpmReady() {
  if (!is_ready_) {
    is_ready_ = (GetFirstByte(kTpmEnabledFile) == "1" &&
                 GetFirstByte(kTpmOwnedFile) == "1");
  }
  return is_ready_;
}

bool TpmUtilityV1::ActivateIdentity(const std::string& delegate_blob,
                                    const std::string& delegate_secret,
                                    const std::string& identity_key_blob,
                                    const std::string& asym_ca_contents,
                                    const std::string& sym_ca_attestation,
                                    std::string* credential) {
  CHECK(credential);

  // Connect to the TPM as the owner delegate.
  ScopedTssContext context_handle_;
  TSS_HTPM tpm_handle;
  if (!ConnectContextAsDelegate(delegate_blob, delegate_secret,
                                &context_handle_, &tpm_handle)) {
    LOG(ERROR) << __func__ << ": Could not connect to the TPM.";
    return false;
  }
  // Load the Storage Root Key.
  TSS_RESULT result;
  ScopedTssKey srk_handle(context_handle_);
  if (!LoadSrk(context_handle_, &srk_handle)) {
    LOG(ERROR) << __func__ << ": Failed to load SRK.";
    return false;
  }
  // Load the AIK (which is wrapped by the SRK).
  std::string mutable_identity_key_blob(identity_key_blob);
  BYTE* identity_key_blob_buffer = reinterpret_cast<BYTE*>(string_as_array(
      &mutable_identity_key_blob));
  ScopedTssKey identity_key(context_handle_);
  result = Tspi_Context_LoadKeyByBlob(
      context_handle_,
      srk_handle,
      identity_key_blob.size(),
      identity_key_blob_buffer,
      identity_key.ptr());
  if (TPM_ERROR(result)) {
    TPM_LOG(ERROR, result) << __func__ << ": Failed to load AIK.";
    return false;
  }
  std::string mutable_asym_ca_contents(asym_ca_contents);
  BYTE* asym_ca_contents_buffer = reinterpret_cast<BYTE*>(string_as_array(
      &mutable_asym_ca_contents));
  std::string mutable_sym_ca_attestation(sym_ca_attestation);
  BYTE* sym_ca_attestation_buffer = reinterpret_cast<BYTE*>(string_as_array(
      &mutable_sym_ca_attestation));
  UINT32 credential_length = 0;
  ScopedTssMemory credential_buffer(context_handle_);
  result = Tspi_TPM_ActivateIdentity(tpm_handle, identity_key,
                                     asym_ca_contents.size(),
                                     asym_ca_contents_buffer,
                                     sym_ca_attestation.size(),
                                     sym_ca_attestation_buffer,
                                     &credential_length,
                                     credential_buffer.ptr());
  if (TPM_ERROR(result)) {
    TPM_LOG(ERROR, result) << __func__ << ": Failed to activate identity.";
    return false;
  }
  credential->assign(reinterpret_cast<const char*>(credential_buffer.value()),
                     credential_length);
  return true;
}

bool TpmUtilityV1::CreateCertifiedKey(KeyType key_type,
                                      KeyUsage key_usage,
                                      const std::string& identity_key_blob,
                                      const std::string& external_data,
                                      std::string* key_blob,
                                      std::string* public_key,
                                      std::string* public_key_tpm_format,
                                      std::string* key_info,
                                      std::string* proof) {
  CHECK(key_blob && public_key && public_key_tpm_format && key_info && proof);
  if (key_type != KEY_TYPE_RSA) {
    LOG(ERROR) << "Only RSA supported on TPM v1.2.";
    return false;
  }

  // Load the AIK (which is wrapped by the SRK).
  ScopedTssKey identity_key(context_handle_);
  if (!LoadKeyFromBlob(identity_key_blob, context_handle_, srk_handle_,
                       &identity_key)) {
    LOG(ERROR) << __func__ << "Failed to load AIK.";
    return false;
  }

  // Create a non-migratable RSA key.
  ScopedTssKey key(context_handle_);
  UINT32 tss_key_type = (key_usage == KEY_USAGE_SIGN) ? TSS_KEY_TYPE_SIGNING :
                                                        TSS_KEY_TYPE_BIND;
  UINT32 init_flags = tss_key_type |
                      TSS_KEY_NOT_MIGRATABLE |
                      TSS_KEY_VOLATILE |
                      TSS_KEY_SIZE_2048;
  TSS_RESULT result = Tspi_Context_CreateObject(context_handle_,
                                                TSS_OBJECT_TYPE_RSAKEY,
                                                init_flags, key.ptr());
  if (TPM_ERROR(result)) {
    TPM_LOG(ERROR, result) << __func__ << ": Failed to create object.";
    return false;
  }
  result = Tspi_SetAttribUint32(key,
                                TSS_TSPATTRIB_KEY_INFO,
                                TSS_TSPATTRIB_KEYINFO_SIGSCHEME,
                                TSS_SS_RSASSAPKCS1V15_DER);
  if (TPM_ERROR(result)) {
    TPM_LOG(ERROR, result) << __func__ << ": Failed to set signature scheme.";
    return false;
  }
  result = Tspi_Key_CreateKey(key, srk_handle_, 0);
  if (TPM_ERROR(result)) {
    TPM_LOG(ERROR, result) << __func__ << ": Failed to create key.";
    return false;
  }
  result = Tspi_Key_LoadKey(key, srk_handle_);
  if (TPM_ERROR(result)) {
    TPM_LOG(ERROR, result) << __func__ << ": Failed to load key.";
    return false;
  }

  // Certify the key.
  TSS_VALIDATION validation;
  memset(&validation, 0, sizeof(validation));
  validation.ulExternalDataLength = external_data.size();
  std::string mutable_external_data(external_data);
  validation.rgbExternalData = reinterpret_cast<BYTE*>(string_as_array(
      &mutable_external_data));
  result = Tspi_Key_CertifyKey(key, identity_key, &validation);
  if (TPM_ERROR(result)) {
    TPM_LOG(ERROR, result) << __func__ << ": Failed to certify key.";
    return false;
  }
  ScopedTssMemory scoped_certified_data(0, validation.rgbData);
  ScopedTssMemory scoped_proof(0, validation.rgbValidationData);

  // Get the certified public key.
  if (!GetDataAttribute(context_handle_,
                        key,
                        TSS_TSPATTRIB_KEY_BLOB,
                        TSS_TSPATTRIB_KEYBLOB_PUBLIC_KEY,
                        public_key_tpm_format)) {
    LOG(ERROR) << __func__ << ": Failed to read public key.";
    return false;
  }
  if (!ConvertPublicKeyToDER(*public_key_tpm_format, public_key)) {
    return false;
  }

  // Get the certified key blob so we can load it later.
  if (!GetDataAttribute(context_handle_,
                        key,
                        TSS_TSPATTRIB_KEY_BLOB,
                        TSS_TSPATTRIB_KEYBLOB_BLOB,
                        key_blob)) {
    LOG(ERROR) << __func__ << ": Failed to read key blob.";
    return false;
  }

  // Get the data that was certified.
  key_info->assign(reinterpret_cast<const char*>(validation.rgbData),
                   validation.ulDataLength);

  // Get the certification proof.
  proof->assign(reinterpret_cast<const char*>(validation.rgbValidationData),
                validation.ulValidationDataLength);
  return true;
}

bool TpmUtilityV1::ConnectContext(ScopedTssContext* context, TSS_HTPM* tpm) {
  *tpm = 0;
  TSS_RESULT result;
  if (TPM_ERROR(result = Tspi_Context_Create(context->ptr()))) {
    TPM_LOG(ERROR, result) << __func__ << ": Error calling Tspi_Context_Create";
    return false;
  }
  if (TPM_ERROR(result = Tspi_Context_Connect(*context, nullptr))) {
    TPM_LOG(ERROR, result) << __func__
                           << ": Error calling Tspi_Context_Connect";
    return false;
  }
  if (TPM_ERROR(result = Tspi_Context_GetTpmObject(*context, tpm))) {
    TPM_LOG(ERROR, result) << __func__
                           << ": Error calling Tspi_Context_GetTpmObject";
    return false;
  }
  return true;
}

bool TpmUtilityV1::ConnectContextAsDelegate(const std::string& delegate_blob,
                                            const std::string& delegate_secret,
                                            ScopedTssContext* context,
                                            TSS_HTPM* tpm) {
  *tpm = 0;
  if (!ConnectContext(context, tpm)) {
    return false;
  }
  TSS_RESULT result;
  TSS_HPOLICY tpm_usage_policy;
  if (TPM_ERROR(result = Tspi_GetPolicyObject(*tpm,
                                              TSS_POLICY_USAGE,
                                              &tpm_usage_policy))) {
    TPM_LOG(ERROR, result) << __func__
                           << ": Error calling Tspi_GetPolicyObject";
    return false;
  }
  std::string mutable_delegate_secret(delegate_secret);
  BYTE* secret_buffer = reinterpret_cast<BYTE*>(string_as_array(
      &mutable_delegate_secret));
  if (TPM_ERROR(result = Tspi_Policy_SetSecret(tpm_usage_policy,
                                               TSS_SECRET_MODE_PLAIN,
                                               delegate_secret.size(),
                                               secret_buffer))) {
    TPM_LOG(ERROR, result) << __func__
                           << ": Error calling Tspi_Policy_SetSecret";
    return false;
  }
  std::string mutable_delegate_blob(delegate_blob);
  BYTE* blob_buffer = reinterpret_cast<BYTE*>(string_as_array(
      &mutable_delegate_blob));
  if (TPM_ERROR(result = Tspi_SetAttribData(
      tpm_usage_policy,
      TSS_TSPATTRIB_POLICY_DELEGATION_INFO,
      TSS_TSPATTRIB_POLDEL_OWNERBLOB,
      delegate_blob.size(),
      blob_buffer))) {
    TPM_LOG(ERROR, result) << __func__ << ": Error calling Tspi_SetAttribData";
    return false;
  }
  return true;
}

bool TpmUtilityV1::LoadSrk(TSS_HCONTEXT context_handle,
                           ScopedTssKey* srk_handle) {
  TSS_RESULT result;
  TSS_UUID uuid = TSS_UUID_SRK;
  if (TPM_ERROR(result = Tspi_Context_LoadKeyByUUID(context_handle_,
                                                    TSS_PS_TYPE_SYSTEM,
                                                    uuid,
                                                    srk_handle->ptr()))) {
    TPM_LOG(ERROR, result) << __func__
                           << ": Error calling Tspi_Context_LoadKeyByUUID";
    return false;
  }
  // Check if the SRK wants a password.
  UINT32 auth_usage;
  if (TPM_ERROR(result = Tspi_GetAttribUint32(*srk_handle,
                                              TSS_TSPATTRIB_KEY_INFO,
                                              TSS_TSPATTRIB_KEYINFO_AUTHUSAGE,
                                              &auth_usage))) {
    TPM_LOG(ERROR, result) << __func__
                           << ": Error calling Tspi_GetAttribUint32";
    return false;
  }
  if (auth_usage) {
    // Give it an empty password if needed.
    TSS_HPOLICY usage_policy;
    if (TPM_ERROR(result = Tspi_GetPolicyObject(*srk_handle,
                                                TSS_POLICY_USAGE,
                                                &usage_policy))) {
    TPM_LOG(ERROR, result) << __func__
                           << ": Error calling Tspi_GetPolicyObject";
      return false;
    }

    BYTE empty_password[] = {};
    if (TPM_ERROR(result = Tspi_Policy_SetSecret(usage_policy,
                                                 TSS_SECRET_MODE_PLAIN,
                                                 0, empty_password))) {
      TPM_LOG(ERROR, result) << __func__
                             << ": Error calling Tspi_Policy_SetSecret";
      return false;
    }
  }
  return true;
}

bool TpmUtilityV1::LoadKeyFromBlob(const std::string& key_blob,
                                   TSS_HCONTEXT context_handle_,
                                   TSS_HKEY parent_key_handle,
                                   ScopedTssKey* key_handle) {
  std::string mutable_key_blob(key_blob);
  BYTE* key_blob_buffer = reinterpret_cast<BYTE*>(string_as_array(
      &mutable_key_blob));
  TSS_RESULT result = Tspi_Context_LoadKeyByBlob(
      context_handle_,
      parent_key_handle,
      key_blob.size(),
      key_blob_buffer,
      key_handle->ptr());
  if (TPM_ERROR(result)) {
    TPM_LOG(ERROR, result) << __func__ << ": Failed to load key by blob.";
    return false;
  }
  return true;
}

bool TpmUtilityV1::GetDataAttribute(TSS_HCONTEXT context,
                                    TSS_HOBJECT object,
                                    TSS_FLAG flag,
                                    TSS_FLAG sub_flag,
                                    std::string* data) {
  UINT32 length = 0;
  ScopedTssMemory buffer(context);
  TSS_RESULT result = Tspi_GetAttribData(object, flag, sub_flag, &length,
                                         buffer.ptr());
  if (TPM_ERROR(result)) {
    TPM_LOG(ERROR, result) << __func__ << "Failed to read object attribute.";
    return false;
  }
  data->assign(reinterpret_cast<const char*>(buffer.value()), length);
  return true;
}

bool TpmUtilityV1::ConvertPublicKeyToDER(const std::string& public_key,
                                         std::string* public_key_der) {
  // Parse the serialized TPM_PUBKEY.
  UINT64 offset = 0;
  std::string mutable_public_key(public_key);
  BYTE* buffer = reinterpret_cast<BYTE*>(string_as_array(&mutable_public_key));
  TPM_PUBKEY parsed;
  TSS_RESULT result = Trspi_UnloadBlob_PUBKEY(&offset, buffer, &parsed);
  if (TPM_ERROR(result)) {
    TPM_LOG(ERROR, result) << "Failed to parse TPM_PUBKEY.";
    return false;
  }
  ScopedByteArray scoped_key(parsed.pubKey.key);
  ScopedByteArray scoped_parms(parsed.algorithmParms.parms);
  TPM_RSA_KEY_PARMS* parms =
      reinterpret_cast<TPM_RSA_KEY_PARMS*>(parsed.algorithmParms.parms);
  crypto::ScopedRSA rsa(RSA_new());
  CHECK(rsa.get());
  // Get the public exponent.
  if (parms->exponentSize == 0) {
    rsa.get()->e = BN_new();
    CHECK(rsa.get()->e);
    BN_set_word(rsa.get()->e, kWellKnownExponent);
  } else {
    rsa.get()->e = BN_bin2bn(parms->exponent, parms->exponentSize, NULL);
    CHECK(rsa.get()->e);
  }
  // Get the modulus.
  rsa.get()->n = BN_bin2bn(parsed.pubKey.key, parsed.pubKey.keyLength, NULL);
  CHECK(rsa.get()->n);

  // DER encode.
  int der_length = i2d_RSAPublicKey(rsa.get(), NULL);
  if (der_length < 0) {
    LOG(ERROR) << "Failed to DER-encode public key.";
    return false;
  }
  public_key_der->resize(der_length);
  unsigned char* der_buffer = reinterpret_cast<unsigned char*>(
      string_as_array(public_key_der));
  der_length = i2d_RSAPublicKey(rsa.get(), &der_buffer);
  if (der_length < 0) {
    LOG(ERROR) << "Failed to DER-encode public key.";
    return false;
  }
  public_key_der->resize(der_length);
  return true;
}


}  // namespace attestation
