// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef RMAD_UTILS_CR50_UTILS_IMPL_H_
#define RMAD_UTILS_CR50_UTILS_IMPL_H_

#include <rmad/utils/cr50_utils.h>

#include <string>

namespace rmad {

class Cr50UtilsImpl : public Cr50Utils {
 public:
  Cr50UtilsImpl() = default;
  ~Cr50UtilsImpl() = default;

  bool RoVerificationKeyPressed() const override;
  bool GetRsuChallenge(std::string* challenge) const override;
};

}  // namespace rmad

#endif  // RMAD_UTILS_CR50_UTILS_IMPL_H_
