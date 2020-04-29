// Copyright 2020 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CRYPTOHOME_KEY_CHALLENGE_SERVICE_FACTORY_H_
#define CRYPTOHOME_KEY_CHALLENGE_SERVICE_FACTORY_H_

#include <memory>
#include <string>

#include "cryptohome/key_challenge_service.h"

namespace cryptohome {

class KeyChallengeServiceFactory {
 public:
  KeyChallengeServiceFactory() = default;
  KeyChallengeServiceFactory(const KeyChallengeServiceFactory&) = delete;
  KeyChallengeServiceFactory& operator=(const KeyChallengeServiceFactory&)
      = delete;
  virtual ~KeyChallengeServiceFactory() = default;

  virtual std::unique_ptr<KeyChallengeService> New(
      const std::string& key_delegate_dbus_service_name) = 0;
};

}  // namespace cryptohome

#endif  // CRYPTOHOME_KEY_CHALLENGE_SERVICE_FACTORY_H_
