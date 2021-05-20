/*
 * Copyright 2021 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "features/hdrnet/hdrnet_ae_device_adapter.h"

#if defined(IPU6EP)
#include "features/hdrnet/hdrnet_ae_device_adapter_ipu6.h"
#endif

namespace cros {

// static
std::unique_ptr<HdrNetAeDeviceAdapter> HdrNetAeDeviceAdapter::CreateInstance() {
#if defined(IPU6EP)
  return std::make_unique<HdrNetAeDeviceAdapterIpu6>();
#else
  return std::make_unique<HdrNetAeDeviceAdapter>();
#endif
}

bool HdrNetAeDeviceAdapter::ExtractAeStats(
    int frame_number,
    const camera_metadata_t* result_metadata,
    MetadataLogger* metadata_logger) {
  return true;
}

bool HdrNetAeDeviceAdapter::HasAeStats(int frame_number) {
  return true;
}

AeParameters HdrNetAeDeviceAdapter::ComputeAeParameters(
    int frame_number, const AeFrameInfo& frame_info, float max_hdr_ratio) {
  return AeParameters();
}

}  // namespace cros
