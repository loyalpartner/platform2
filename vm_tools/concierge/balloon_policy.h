// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef VM_TOOLS_CONCIERGE_BALLOON_POLICY_H_
#define VM_TOOLS_CONCIERGE_BALLOON_POLICY_H_

#include <stdint.h>
#include <string>

namespace vm_tools {
namespace concierge {

constexpr int64_t KIB = 1024;
constexpr int64_t MIB = 1024 * 1024;

struct BalloonStats {
  int64_t available_memory;
  int64_t balloon_actual;
  int64_t disk_caches;
  int64_t free_memory;
  int64_t major_faults;
  int64_t minor_faults;
  int64_t swap_in;
  int64_t swap_out;
  int64_t total_memory;
};

struct MemoryMargins {
  uint64_t critical;
  uint64_t moderate;
};

class BalloonPolicyInterface {
 public:
  virtual ~BalloonPolicyInterface() {}

  // Calculates the amount of memory to be shifted between a VM and the host.
  // Positive value means that the policy wants to move that amount of memory
  // from the guest to the host.
  virtual int64_t ComputeBalloonDelta(const BalloonStats& stats,
                                      uint64_t host_available,
                                      bool game_mode,
                                      const std::string& vm) = 0;
};

class BalanceAvailableBalloonPolicy : public BalloonPolicyInterface {
 public:
  BalanceAvailableBalloonPolicy(int64_t critical_host_available,
                                int64_t guest_available_bias,
                                const std::string& vm);

  int64_t ComputeBalloonDelta(const BalloonStats& stats,
                              uint64_t host_available,
                              bool game_mode,
                              const std::string& vm) override;

 private:
  // ChromeOS's critical margin.
  const int64_t critical_host_available_;

  // How much to bias the balance of available memory, depending on how full
  // the balloon is.
  const int64_t guest_available_bias_;

  // The max actual balloon size observed.
  int64_t max_balloon_actual_;

  // This is a guessed value of guest's critical available
  // size. If free memory is smaller than this, guest memory
  // managers (OOM, Android LMKD) will start killing apps.
  int64_t critical_guest_available_;

  // for calculating critical_guest_available
  int64_t prev_guest_available_;
  int64_t prev_balloon_full_percent_;

  // This class keeps the state of a balloon and modified only via
  // ComputeBalloonDelta() so no copy/assign operations are needed.
  BalanceAvailableBalloonPolicy(const BalanceAvailableBalloonPolicy&) = delete;
  BalanceAvailableBalloonPolicy& operator=(
      const BalanceAvailableBalloonPolicy&) = delete;
};

}  // namespace concierge
}  // namespace vm_tools

#endif  // VM_TOOLS_CONCIERGE_BALLOON_POLICY_H_
