// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SHILL_CELLULAR_MOCK_MODEM_INFO_H_
#define SHILL_CELLULAR_MOCK_MODEM_INFO_H_

#include <memory>
#include <string>

#include <base/macros.h>
#include <gmock/gmock.h>

#include "shill/cellular/mock_pending_activation_store.h"
#include "shill/cellular/modem_info.h"

namespace shill {

class MockModemInfo : public ModemInfo {
 public:
  MockModemInfo(ControlInterface* control, Manager* manager);

  ~MockModemInfo() override;

  MockPendingActivationStore* mock_pending_activation_store() const {
    return mock_pending_activation_store_;
  }

  MOCK_METHOD(void, Start, (), (override));
  MOCK_METHOD(void, Stop, (), (override));
  MOCK_METHOD(void, OnDeviceInfoAvailable, (const std::string&), (override));

 private:
  // owned by ModemInfo
  MockPendingActivationStore* mock_pending_activation_store_;

  DISALLOW_COPY_AND_ASSIGN(MockModemInfo);
};

}  // namespace shill

#endif  // SHILL_CELLULAR_MOCK_MODEM_INFO_H_
