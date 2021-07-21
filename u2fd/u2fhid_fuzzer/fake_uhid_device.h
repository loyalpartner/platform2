// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef U2FD_U2FHID_FUZZER_FAKE_UHID_DEVICE_H_
#define U2FD_U2FHID_FUZZER_FAKE_UHID_DEVICE_H_

#include <string>

#include "u2fd/hid_interface.h"

namespace u2f {

class FakeUHidDevice : public HidInterface {
 public:
  FakeUHidDevice() = default;
  FakeUHidDevice(const FakeUHidDevice&) = delete;
  FakeUHidDevice& operator=(const FakeUHidDevice&) = delete;

  // Sending a report to the output report callback.
  // |report| should be generated by the fuzzing engine.
  void SendOutputReport(const std::string& report);

  // HidInterface implementation:
  bool Init(uint32_t hid_version, const std::string& report_desc) override;
  bool SendReport(const std::string& report) override;
  void SetOutputReportHandler(
      const HidInterface::OutputReportCallback& on_output_report) override;

 private:
  HidInterface::OutputReportCallback on_output_report_;
};

}  // namespace u2f

#endif  // U2FD_U2FHID_FUZZER_FAKE_UHID_DEVICE_H_