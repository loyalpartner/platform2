// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shill/diagnostics_reporter.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "shill/mock_glib.h"
#include "shill/mock_time.h"

using testing::_;
using testing::Return;
using testing::SetArgumentPointee;

namespace shill {

namespace {

class ReporterUnderTest : public DiagnosticsReporter {
 public:
  ReporterUnderTest() {}

  MOCK_METHOD0(IsReportingEnabled, bool());

  void Report() { DiagnosticsReporter::Report(); }

 private:
  DISALLOW_COPY_AND_ASSIGN(ReporterUnderTest);
};

}  // namespace

class DiagnosticsReporterTest : public testing::Test {
 public:
  DiagnosticsReporterTest() {
    reporter_.time_ = &time_;
    reporter_.Init(&glib_);
  }

 protected:
  bool IsReportingEnabled() {
    return DiagnosticsReporter::GetInstance()->IsReportingEnabled();
  }

  void SetLastLogStash(uint64 time) { reporter_.last_log_stash_ = time; }
  uint64 GetLastLogStash() { return reporter_.last_log_stash_; }
  uint64 GetLogStashThrottleSeconds() {
    return DiagnosticsReporter::kLogStashThrottleSeconds;
  }

  MockGLib glib_;
  MockTime time_;
  ReporterUnderTest reporter_;
};

TEST_F(DiagnosticsReporterTest, ReportEnabled) {
  EXPECT_CALL(reporter_, IsReportingEnabled()).WillOnce(Return(true));
  EXPECT_CALL(glib_, SpawnSync(_, _, _, _, _, _, _, _, _, _)).Times(1);
  reporter_.Report();
}

TEST_F(DiagnosticsReporterTest, ReportDisabled) {
  EXPECT_CALL(reporter_, IsReportingEnabled()).WillOnce(Return(false));
  EXPECT_CALL(glib_, SpawnSync(_, _, _, _, _, _, _, _, _, _)).Times(0);
  reporter_.Report();
}

TEST_F(DiagnosticsReporterTest, IsReportingEnabled) {
  EXPECT_FALSE(IsReportingEnabled());
}

TEST_F(DiagnosticsReporterTest, OnConnectivityEventThrottle) {
  const uint64 kLastStash = 50;
  const uint64 kNow = kLastStash + GetLogStashThrottleSeconds() - 1;
  SetLastLogStash(kLastStash);
  const struct timeval now = {
    .tv_sec = static_cast<long int>(kNow),
    .tv_usec = 0
  };
  EXPECT_CALL(time_, GetTimeMonotonic(_))
      .WillOnce(DoAll(SetArgumentPointee<0>(now), Return(0)));
  reporter_.OnConnectivityEvent();
  EXPECT_EQ(kLastStash, GetLastLogStash());
}

TEST_F(DiagnosticsReporterTest, OnConnectivityEvent) {
  const uint64 kLastStash = 50;
  const uint64 kNow = kLastStash + GetLogStashThrottleSeconds() + 1;
  SetLastLogStash(kLastStash);
  const struct timeval now = {
    .tv_sec = static_cast<long int>(kNow),
    .tv_usec = 0
  };
  EXPECT_CALL(time_, GetTimeMonotonic(_))
      .WillOnce(DoAll(SetArgumentPointee<0>(now), Return(0)));
  reporter_.OnConnectivityEvent();
  EXPECT_EQ(kNow, GetLastLogStash());
}

}  // namespace shill
