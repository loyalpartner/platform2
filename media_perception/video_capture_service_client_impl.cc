// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media_perception/video_capture_service_client_impl.h"

#include <utility>
#include <base/single_thread_task_runner.h>

#include "media_perception/device_management.pb.h"
#include "media_perception/proto_mojom_conversion.h"

namespace mri {

void VideoCaptureServiceClientImpl::SetMojoConnector(
    MojoConnector* mojo_connector) {
  mojo_connector_ = mojo_connector;
}

bool VideoCaptureServiceClientImpl::Connect() {
  if (mojo_connector_ == nullptr) {
    LOG(ERROR) << "Mojo connector is nullptr.";
    return false;
  }
  mojo_connector_->ConnectToVideoCaptureService();
  return true;
}

bool VideoCaptureServiceClientImpl::IsConnected() {
  if (mojo_connector_ == nullptr)
    return false;

  return mojo_connector_->IsConnectedToVideoCaptureService();
}

void VideoCaptureServiceClientImpl::GetDevices(
    const GetDevicesCallback& callback) {
  mojo_connector_->GetDevices(callback);
}

void VideoCaptureServiceClientImpl::OpenDevice(
    const std::string& device_id, const OpenDeviceCallback& callback) {
  mojo_connector_->OpenDevice(device_id, callback);
}

bool VideoCaptureServiceClientImpl::IsVideoCaptureStartedForDevice(
    const std::string& device_id,
    SerializedVideoStreamParams* capture_format) {
  std::lock_guard<std::mutex> lock(device_id_to_receiver_map_lock_);
  std::map<std::string, std::shared_ptr<ReceiverImpl>>::iterator it =
      device_id_to_receiver_map_.find(device_id);
  bool capture_started = it != device_id_to_receiver_map_.end() &&
      it->second->HasValidCaptureFormat();
  if (capture_started) {
    *capture_format = SerializeVideoStreamParamsProto(
        it->second->GetCaptureFormat());
  }
  return capture_started;
}

int VideoCaptureServiceClientImpl::AddFrameHandler(
    const std::string& device_id,
    const SerializedVideoStreamParams& capture_format,
    FrameHandler handler) {
  std::lock_guard<std::mutex> lock(device_id_to_receiver_map_lock_);
  VideoStreamParams format;
  CHECK(format.ParseFromArray(capture_format.data(), capture_format.size()))
      << "Failed to deserialize mri::VideoStreamParams proto.";

  std::map<std::string, std::shared_ptr<ReceiverImpl>>::iterator it =
      device_id_to_receiver_map_.find(device_id);
  if (it != device_id_to_receiver_map_.end() &&
      it->second->HasValidCaptureFormat()) {
    LOG(INFO) << "Device with " << device_id << " already open.";
    if (!it->second->CaptureFormatsMatch(format)) {
      LOG(WARNING) << "Device " << device_id << " is already open but with "
                   << "different capture formats.";
      return 0;
    }
    return it->second->AddFrameHandler(std::move(handler));
  }

  std::shared_ptr<ReceiverImpl> receiver_impl;
  if (it != device_id_to_receiver_map_.end()) {
    receiver_impl = it->second;
  } else {  // Create receiver if it doesn't exist.
    receiver_impl = std::make_shared<ReceiverImpl>();
  }
  receiver_impl->SetCaptureFormat(format);
  device_id_to_receiver_map_.insert(
      std::make_pair(device_id, receiver_impl));
  mojo_connector_->StartVideoCapture(
      device_id, receiver_impl, format);
  return receiver_impl->AddFrameHandler(std::move(handler));
}

bool VideoCaptureServiceClientImpl::RemoveFrameHandler(
    const std::string& device_id, int frame_handler_id) {
  std::lock_guard<std::mutex> lock(device_id_to_receiver_map_lock_);
  std::map<std::string, std::shared_ptr<ReceiverImpl>>::iterator it =
      device_id_to_receiver_map_.find(device_id);

  if (it == device_id_to_receiver_map_.end()) {
    // Receiver does not exist. Ensure that the device is removed as well.
    mojo_connector_->StopVideoCapture(device_id);
    return false;
  }

  // Receiver does exist.
  bool success = it->second->RemoveFrameHandler(frame_handler_id);
  if (it->second->GetFrameHandlerCount() == 0) {
    // Remove the receiver object.
    device_id_to_receiver_map_.erase(device_id);
    // Stop video capture on the device.
    mojo_connector_->StopVideoCapture(device_id);
  }
  return success;
}

void VideoCaptureServiceClientImpl::CreateVirtualDevice(
    const SerializedVideoDevice& video_device,
    const VirtualDeviceCallback& callback) {
  std::lock_guard<std::mutex> lock(device_id_to_producer_map_lock_);
  VideoDevice device;
  CHECK(device.ParseFromArray(video_device.data(), video_device.size()))
      << "Failed to deserialze mri::VideoDevice proto.";

  auto producer_impl = std::make_shared<ProducerImpl>();
  mojo_connector_->CreateVirtualDevice(device, producer_impl, callback);

  device_id_to_producer_map_.insert(
      std::make_pair(device.id(), producer_impl));
}

void VideoCaptureServiceClientImpl::PushFrameToVirtualDevice(
    const std::string& device_id, uint64_t timestamp_in_microseconds,
    std::unique_ptr<const uint8_t[]> data, int data_size,
    RawPixelFormat pixel_format, int frame_width, int frame_height) {
  std::lock_guard<std::mutex> lock(device_id_to_producer_map_lock_);
  std::map<std::string, std::shared_ptr<ProducerImpl>>::iterator it =
      device_id_to_producer_map_.find(device_id);
  if (it == device_id_to_producer_map_.end()) {
    LOG(ERROR) << "Device id not found in producer map.";
    return;
  }
  mojo_connector_->PushFrameToVirtualDevice(
      it->second, base::TimeDelta::FromMicroseconds(timestamp_in_microseconds),
      std::move(data), data_size, static_cast<PixelFormat>(pixel_format),
      frame_width, frame_height);
}

void VideoCaptureServiceClientImpl::CloseVirtualDevice(
    const std::string& device_id) {
  std::lock_guard<std::mutex> lock(device_id_to_producer_map_lock_);
  // Erasing the producer object will close the virtual device.
  device_id_to_producer_map_.erase(device_id);
}

}  // namespace mri
