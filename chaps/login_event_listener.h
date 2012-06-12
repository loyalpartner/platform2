// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHAPS_LOGIN_EVENT_LISTENER_H
#define CHAPS_LOGIN_EVENT_LISTENER_H

#include <string>

#include <base/file_path.h>
#include <chromeos/secure_blob.h>

namespace chaps {

// LoginEventListener is an interface which must be implemented by objects which
// register to receive notification of login / logout events.
//
//   An event is always parameterized with a path to the associated persistent
//   token files. This path is unique per token and a token is unique per path.
//   This 1-to-1 relation can be assumed.
//
//   Authorization data associated with a token is derived from the user's
//   password and is provided via this interface on login events and on change
//   password events.
class LoginEventListener {
 public:
  virtual void OnLogin(const FilePath& path,
                       const chromeos::SecureBlob& auth_data) = 0;
  virtual void OnLogout(const FilePath& path) = 0;
  virtual void OnChangeAuthData(const FilePath& path,
                                const chromeos::SecureBlob& old_auth_data,
                                const chromeos::SecureBlob& new_auth_data) = 0;
};

}  // namespace

#endif  // CHAPS_LOGIN_EVENT_LISTENER_H
