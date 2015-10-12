//
// Copyright (C) 2015 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#ifndef TPM_MANAGER_COMMON_TPM_MANAGER_INTERFACE_H_
#define TPM_MANAGER_COMMON_TPM_MANAGER_INTERFACE_H_

#include <base/callback.h>

#include "tpm_manager/common/export.h"
#include "tpm_manager/common/tpm_nvram_interface.h"
#include "tpm_manager/common/tpm_ownership_interface.h"

namespace tpm_manager {

// This is the main TpmManager interface that is implemented by the proxies
// and services.
// TODO(usanghi): Move this class into server/ since the client side will
// implement each interface seperately.
class TPM_MANAGER_EXPORT TpmManagerInterface : public TpmNvramInterface,
                                               public TpmOwnershipInterface {
 public:
  virtual ~TpmManagerInterface() = default;

  // Performs initialization tasks. This method must be called before calling
  // any other method on this interface.
  virtual bool Initialize() = 0;

  // TpmOwnershipInterface methods.
  virtual void GetTpmStatus(const GetTpmStatusRequest& request,
                            const GetTpmStatusCallback& callback) = 0;
  virtual void TakeOwnership(const TakeOwnershipRequest& request,
                             const TakeOwnershipCallback& callback) = 0;

  // TpmNvramInterface methods.
  virtual void DefineNvram(const DefineNvramRequest& request,
                           const DefineNvramCallback& callback) = 0;
  virtual void DestroyNvram(const DestroyNvramRequest& request,
                            const DestroyNvramCallback& callback) = 0;
  virtual void WriteNvram(const WriteNvramRequest& request,
                          const WriteNvramCallback& callback) = 0;
  virtual void ReadNvram(const ReadNvramRequest& request,
                         const ReadNvramCallback& callback) = 0;
  virtual void IsNvramDefined(const IsNvramDefinedRequest& request,
                              const IsNvramDefinedCallback& callback) = 0;
  virtual void IsNvramLocked(const IsNvramLockedRequest& request,
                             const IsNvramLockedCallback& callback) = 0;
  virtual void GetNvramSize(const GetNvramSizeRequest& request,
                            const GetNvramSizeCallback& callback) = 0;
};

}  // namespace tpm_manager

#endif  // TPM_MANAGER_COMMON_TPM_MANAGER_INTERFACE_H_
