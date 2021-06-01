// Copyright 2020 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DIAGNOSTICS_CROS_HEALTHD_FETCHERS_MEMORY_FETCHER_H_
#define DIAGNOSTICS_CROS_HEALTHD_FETCHERS_MEMORY_FETCHER_H_

#include "diagnostics/cros_healthd/system/context.h"
#include "mojo/cros_healthd_probe.mojom.h"

namespace diagnostics {

// The MemoryFetcher class is responsible for gathering memory info.
class MemoryFetcher final {
 public:
  explicit MemoryFetcher(Context* context);
  MemoryFetcher(const MemoryFetcher&) = delete;
  MemoryFetcher& operator=(const MemoryFetcher&) = delete;
  ~MemoryFetcher() = default;

  // Returns a structure with either the device's memory info or the error that
  // occurred fetching the information.
  chromeos::cros_healthd::mojom::MemoryResultPtr FetchMemoryInfo();

 private:
  using OptionalProbeErrorPtr =
      base::Optional<chromeos::cros_healthd::mojom::ProbeErrorPtr>;

  OptionalProbeErrorPtr ParseProcMeminfo(
      chromeos::cros_healthd::mojom::MemoryInfo* info);
  OptionalProbeErrorPtr ParseProcVmStat(
      chromeos::cros_healthd::mojom::MemoryInfo* info);

  // Unowned pointer that outlives this MemoryFetcher instance.
  Context* const context_ = nullptr;
};

}  // namespace diagnostics

#endif  // DIAGNOSTICS_CROS_HEALTHD_FETCHERS_MEMORY_FETCHER_H_
