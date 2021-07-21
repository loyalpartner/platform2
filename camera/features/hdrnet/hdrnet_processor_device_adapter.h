/*
 * Copyright 2021 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef CAMERA_FEATURES_HDRNET_HDRNET_PROCESSOR_DEVICE_ADAPTER_H_
#define CAMERA_FEATURES_HDRNET_HDRNET_PROCESSOR_DEVICE_ADAPTER_H_

#include <memory>

#include <base/single_thread_task_runner.h>
#include <system/camera_metadata.h>

#include "features/hdrnet/hdrnet_config.h"
#include "gpu/shared_image.h"

namespace cros {

// Device specilization for the pre-processing and post-processing of the HDRnet
// pipeline.
//
// The default HdrNetProcessorDeviceAdapter implementation does nothing.
class HdrNetProcessorDeviceAdapter {
 public:
  static std::unique_ptr<HdrNetProcessorDeviceAdapter> CreateInstance(
      const camera_metadata_t* static_info,
      scoped_refptr<base::SingleThreadTaskRunner> task_runner);

  virtual ~HdrNetProcessorDeviceAdapter() = default;
  virtual bool Initialize();
  virtual void TearDown();

  // Called on every frame with the per-frame capture result metadata.
  virtual void ProcessResultMetadata(int frame_number,
                                     const camera_metadata_t* metadata);

  // Called by HdrNetProcessorImpl::Run() to convert the device-specific YUV
  // buffers generated by the ISP to linear RGB images.
  virtual bool Preprocess(const HdrNetConfig::Options& options,
                          const SharedImage& input_external_yuv,
                          const SharedImage& output_rgba);

  // Called by HdrNetProcessorImpl::Run() to convert the RGB images rendered by
  // the HDRnet pipeline to the NV12 buffer the client expects.
  virtual bool Postprocess(const HdrNetConfig::Options& options,
                           const SharedImage& input_rgba,
                           const SharedImage& output_nv12);
};

}  // namespace cros

#endif  // CAMERA_FEATURES_HDRNET_HDRNET_PROCESSOR_DEVICE_ADAPTER_H_