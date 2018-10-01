// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media_perception/proto_mojom_conversion.h"

#include <utility>

namespace chromeos {
namespace media_perception {
namespace mojom {

PixelFormat ToMojom(mri::PixelFormat format) {
  switch (format) {
    case mri::PixelFormat::I420:
      return PixelFormat::I420;
    case mri::PixelFormat::MJPEG:
      return PixelFormat::MJPEG;
    case mri::PixelFormat::FORMAT_UNKNOWN:
      return PixelFormat::FORMAT_UNKNOWN;
  }
  return PixelFormat::FORMAT_UNKNOWN;
}

VideoStreamParamsPtr ToMojom(const mri::VideoStreamParams& params) {
  VideoStreamParamsPtr params_ptr = VideoStreamParams::New();
  params_ptr->width_in_pixels = params.width_in_pixels();
  params_ptr->height_in_pixels = params.height_in_pixels();
  params_ptr->frame_rate_in_frames_per_second =
      params.frame_rate_in_frames_per_second();
  params_ptr->pixel_format = ToMojom(params.pixel_format());
  return params_ptr;
}

VideoDevicePtr ToMojom(const mri::VideoDevice& device) {
  VideoDevicePtr device_ptr = VideoDevice::New();
  device_ptr->id = device.id();
  device_ptr->display_name = device.display_name();
  device_ptr->model_id = device.model_id();
  mojo::Array<VideoStreamParamsPtr> supported_configurations;
  for (const mri::VideoStreamParams& params :
       device.supported_configurations()) {
    supported_configurations.push_back(ToMojom(params));
  }
  device_ptr->supported_configurations = std::move(supported_configurations);
  if (device.has_configuration()) {
    device_ptr->configuration = ToMojom(device.configuration());
  }
  device_ptr->in_use = device.in_use();
  return device_ptr;
}

VirtualVideoDevicePtr ToMojom(const mri::VirtualVideoDevice& device) {
  VirtualVideoDevicePtr device_ptr = VirtualVideoDevice::New();
  if (device.has_video_device())
    device_ptr->video_device = ToMojom(device.video_device());
  return device_ptr;
}

AudioStreamParamsPtr ToMojom(const mri::AudioStreamParams& params) {
  AudioStreamParamsPtr params_ptr = AudioStreamParams::New();
  params_ptr->frequency_in_hz = params.frequency_in_hz();
  params_ptr->num_channels = params.num_channels();
  return params_ptr;
}

AudioDevicePtr ToMojom(const mri::AudioDevice& device) {
  AudioDevicePtr device_ptr = AudioDevice::New();
  device_ptr->id = device.id();
  device_ptr->display_name = device.display_name();
  mojo::Array<AudioStreamParamsPtr> supported_configurations;
  for (const mri::AudioStreamParams& params :
       device.supported_configurations()) {
    supported_configurations.push_back(ToMojom(params));
  }
  device_ptr->supported_configurations = std::move(supported_configurations);
  if (device.has_configuration()) {
    device_ptr->configuration = ToMojom(device.configuration());
  }
  return device_ptr;
}

DeviceType ToMojom(mri::DeviceType type) {
  switch (type) {
    case mri::DeviceType::VIDEO:
      return DeviceType::VIDEO;
    case mri::DeviceType::AUDIO:
      return DeviceType::AUDIO;
    case mri::DeviceType::VIRTUAL_VIDEO:
      return DeviceType::VIRTUAL_VIDEO;
    case mri::DeviceType::DEVICE_TYPE_UNKNOWN:
      return DeviceType::TYPE_UNKNOWN;
  }
  return DeviceType::TYPE_UNKNOWN;
}

DeviceTemplatePtr ToMojom(const mri::DeviceTemplate& device_template) {
  DeviceTemplatePtr template_ptr = DeviceTemplate::New();
  template_ptr->template_name = device_template.template_name();
  template_ptr->device_type = ToMojom(device_template.device_type());
  return template_ptr;
}

}  // namespace mojom
}  // namespace media_perception
}  // namespace chromeos

namespace mri {

PixelFormat ToProto(
    chromeos::media_perception::mojom::PixelFormat format) {
  switch (format) {
    case chromeos::media_perception::mojom::PixelFormat::I420:
      return PixelFormat::I420;
    case chromeos::media_perception::mojom::PixelFormat::MJPEG:
      return PixelFormat::MJPEG;
    case chromeos::media_perception::mojom::PixelFormat::FORMAT_UNKNOWN:
      return PixelFormat::FORMAT_UNKNOWN;
  }
  return PixelFormat::FORMAT_UNKNOWN;
}

VideoStreamParams ToProto(
    const chromeos::media_perception::mojom::VideoStreamParamsPtr& params_ptr) {
  VideoStreamParams params;
  if (params_ptr.is_null())
    return params;
  params.set_width_in_pixels(params_ptr->width_in_pixels);
  params.set_height_in_pixels(params_ptr->height_in_pixels);
  params.set_frame_rate_in_frames_per_second(
      params_ptr->frame_rate_in_frames_per_second);
  params.set_pixel_format(
      ToProto(params_ptr->pixel_format));
  return params;
}

VideoDevice ToProto(
    const chromeos::media_perception::mojom::VideoDevicePtr& device_ptr) {
  VideoDevice device;
  if (device_ptr.is_null())
    return device;
  device.set_id(device_ptr->id);
  device.set_display_name(device_ptr->display_name);
  device.set_model_id(device_ptr->model_id);
  for (int i = 0; i < device_ptr->supported_configurations.size(); i++) {
    mri::VideoStreamParams* params = device.add_supported_configurations();
    *params = ToProto(device_ptr->supported_configurations[i]);
  }
  if (!device_ptr->configuration.is_null()) {
    mri::VideoStreamParams* params = device.mutable_configuration();
    *params = ToProto(device_ptr->configuration);
  }
  device.set_in_use(device_ptr->in_use);
  return device;
}

VirtualVideoDevice ToProto(
    const chromeos::media_perception::mojom::VirtualVideoDevicePtr&
    device_ptr) {
  VirtualVideoDevice device;
  if (device_ptr.is_null())
    return device;

  VideoDevice* video_device = device.mutable_video_device();
  *video_device = ToProto(device_ptr->video_device);
  return device;
}

AudioStreamParams ToProto(
    const chromeos::media_perception::mojom::AudioStreamParamsPtr&
    params_ptr) {
  AudioStreamParams params;
  if (params_ptr.is_null())
    return params;

  params.set_frequency_in_hz(params_ptr->frequency_in_hz);
  params.set_num_channels(params_ptr->num_channels);
  return params;
}

AudioDevice ToProto(
    const chromeos::media_perception::mojom::AudioDevicePtr& device_ptr) {
  AudioDevice device;
  if (device_ptr.is_null())
    return device;

  device.set_id(device_ptr->id);
  device.set_display_name(device_ptr->display_name);
  for (int i = 0; i < device_ptr->supported_configurations.size(); i++) {
    mri::AudioStreamParams* params = device.add_supported_configurations();
    *params = ToProto(device_ptr->supported_configurations[i]);
  }
  if (!device_ptr->configuration.is_null()) {
    mri::AudioStreamParams* params = device.mutable_configuration();
    *params = ToProto(device_ptr->configuration);
  }
  return device;
}

DeviceType ToProto(
    const chromeos::media_perception::mojom::DeviceType type) {
  switch (type) {
    case chromeos::media_perception::mojom::DeviceType::VIDEO:
      return DeviceType::VIDEO;
    case chromeos::media_perception::mojom::DeviceType::AUDIO:
      return DeviceType::AUDIO;
    case chromeos::media_perception::mojom::DeviceType::VIRTUAL_VIDEO:
      return DeviceType::VIRTUAL_VIDEO;
    case chromeos::media_perception::mojom::DeviceType::TYPE_UNKNOWN:
      return DeviceType::DEVICE_TYPE_UNKNOWN;
  }
  return DeviceType::DEVICE_TYPE_UNKNOWN;
}

DeviceTemplate ToProto(
    const chromeos::media_perception::mojom::DeviceTemplatePtr& template_ptr) {
  DeviceTemplate device_template;
  if (template_ptr.is_null())
    return device_template;

  device_template.set_template_name(template_ptr->template_name);
  device_template.set_device_type(ToProto(template_ptr->device_type));
  return device_template;
}

}  // namespace mri
