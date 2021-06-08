// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "power_manager/powerd/system/ambient_light_sensor_delegate_mojo.h"

#include <memory>
#include <utility>
#include <vector>

#include <base/check.h>
#include <base/check_op.h>
#include <base/run_loop.h>
#include <gtest/gtest.h>

#include "power_manager/powerd/system/ambient_light_observer.h"
#include "power_manager/powerd/system/fake_sensor_device.h"

namespace power_manager {
namespace system {

namespace {

const int kFakeSensorId = 1;

// Simple AmbientLightObserver implementation that stores the
// AmbientLightSensor* upon samples and helps check the result.
class TestObserver : public AmbientLightObserver {
 public:
  TestObserver(const TestObserver&) = delete;
  TestObserver& operator=(const TestObserver&) = delete;

  TestObserver() = default;
  ~TestObserver() override = default;

  void CheckSample(int lux) {
    EXPECT_TRUE(sensor_);
    EXPECT_EQ(sensor_->GetAmbientLightLux(), lux);

    sensor_ = nullptr;
  }

  void CheckSample(int lux, int temperature) {
    EXPECT_TRUE(sensor_);
    EXPECT_EQ(sensor_->GetAmbientLightLux(), lux);
    EXPECT_EQ(sensor_->GetColorTemperature(), temperature);

    sensor_ = nullptr;
  }

  // AmbientLightObserver implementation:
  void OnAmbientLightUpdated(AmbientLightSensorInterface* sensor) override {
    sensor_ = sensor;
  }

 private:
  AmbientLightSensorInterface* sensor_ = nullptr;
};

}  // namespace

class AmbientLightSensorDelegateMojoTest : public ::testing::Test {
 public:
  AmbientLightSensorDelegateMojoTest(
      const AmbientLightSensorDelegateMojoTest&) = delete;
  AmbientLightSensorDelegateMojoTest& operator=(
      const AmbientLightSensorDelegateMojoTest&) = delete;

  AmbientLightSensorDelegateMojoTest() {}
  ~AmbientLightSensorDelegateMojoTest() override {}

 protected:
  void SetUp() override {
    sensor_ = std::make_unique<system::AmbientLightSensor>();
    sensor_->AddObserver(&observer_);
  }

  void TearDown() override { sensor_->RemoveObserver(&observer_); }

  void InitSensor(bool color_delegate, bool fake_color_sensor) {
    sensor_device_ = std::make_unique<FakeSensorDevice>(
        fake_color_sensor, /*name=*/base::nullopt, /*location=*/base::nullopt);

    base::RunLoop loop;

    mojo::Remote<cros::mojom::SensorDevice> remote;
    sensor_device_->AddReceiver(remote.BindNewPipeAndPassReceiver());
    auto light = AmbientLightSensorDelegateMojo::Create(
        kFakeSensorId, std::move(remote), color_delegate, loop.QuitClosure());
    light_ = light.get();
    CHECK(light);
    sensor_->SetDelegate(std::move(light));

    // Wait until all initialization steps are done.
    loop.Run();
  }

  void WriteLux(int64_t lux) {
    base::flat_map<int32_t, int64_t> sample;
    sample[0] = lux;

    light_->OnSampleUpdated(std::move(sample));
  }

  // The indices of [0, 1, 2, 3] imply channels [lux, ChannelType::X,
  // ChannelType::Y, ChannelType::Z].
  void WriteColorLux(int64_t lux, std::vector<int64_t> color_lux) {
    CHECK_EQ(color_lux.size(), base::size(kColorChannelConfig));

    base::flat_map<int32_t, int64_t> sample;
    sample[0] = lux;
    for (size_t i = 0; i < color_lux.size(); ++i)
      sample[i + 1] = color_lux[i];

    light_->OnSampleUpdated(std::move(sample));
  }

  TestObserver observer_;

  std::unique_ptr<FakeSensorDevice> sensor_device_;

  std::unique_ptr<AmbientLightSensor> sensor_;
  AmbientLightSensorDelegateMojo* light_;
};

TEST_F(AmbientLightSensorDelegateMojoTest, NoColorSensor) {
  InitSensor(/*color_delegate=*/false, /*fake_color_sensor=*/false);

  EXPECT_FALSE(sensor_->IsColorSensor());

  WriteLux(100);
  observer_.CheckSample(100);

  WriteLux(200);
  observer_.CheckSample(200);

  // Simulate disconnection of the observer channel.
  sensor_device_->StopReadingSamples();

  // Wait until the disconnection is done.
  base::RunLoop().RunUntilIdle();

  // OnObserverDisconnect shouldn't reset SensorDevice's mojo endpoint so that
  // AmbientLightSensorManager can get the disconnection.
  EXPECT_TRUE(sensor_device_->HasReceivers());
}

TEST_F(AmbientLightSensorDelegateMojoTest, NoColorDelegateOnColorSensor) {
  InitSensor(/*color_delegate=*/false, /*fake_color_sensor=*/true);

  EXPECT_FALSE(sensor_->IsColorSensor());
}

TEST_F(AmbientLightSensorDelegateMojoTest, ColorDelegateOnNoColorSensor) {
  InitSensor(/*color_delegate=*/true, /*fake_color_sensor=*/false);

  EXPECT_FALSE(sensor_->IsColorSensor());
}

TEST_F(AmbientLightSensorDelegateMojoTest, ColorSensor) {
  InitSensor(/*color_delegate=*/true, /*fake_color_sensor=*/true);

  WriteLux(100);
  observer_.CheckSample(100);

  WriteColorLux(40, std::vector<int64_t>{50, 50, 100});
  observer_.CheckSample(40, 20921);

  EXPECT_TRUE(sensor_->IsColorSensor());

  WriteLux(100);
  // Previous color temperature still remains.
  observer_.CheckSample(100, 20921);

  WriteColorLux(55, std::vector<int64_t>{50, 60, 60});
  observer_.CheckSample(55, 7253);

  EXPECT_TRUE(sensor_->IsColorSensor());
}

TEST_F(AmbientLightSensorDelegateMojoTest, GiveUpAfterTooManyFailures) {
  InitSensor(/*color_delegate=*/false, /*fake_color_sensor=*/false);

  EXPECT_FALSE(sensor_->IsColorSensor());

  for (uint32_t i = 0;
       i < AmbientLightSensorDelegateMojo::kNumFailedReadsBeforeGivingUp - 1;
       ++i) {
    light_->OnErrorOccurred(cros::mojom::ObserverErrorType::READ_FAILED);
  }

  // |num_failed_reads_| is recovered by 1.
  for (uint32_t i = 0; i < AmbientLightSensorDelegateMojo::kNumRecoveryReads;
       ++i)
    WriteLux(100);

  observer_.CheckSample(100);

  // The additional read failures make the delegate give up reading samples.
  light_->OnErrorOccurred(cros::mojom::ObserverErrorType::READ_FAILED);
  light_->OnErrorOccurred(cros::mojom::ObserverErrorType::READ_FAILED);

  // Wait until |sensor_device_| is disconnected.
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(sensor_device_->HasReceivers());
}

}  // namespace system
}  // namespace power_manager