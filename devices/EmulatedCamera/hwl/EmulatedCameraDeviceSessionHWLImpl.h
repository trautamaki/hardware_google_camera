/*
 * Copyright (C) 2013-2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef EMULATOR_CAMERA_HAL_HWL_CAMERA_DEVICE_SESSION_HWL_IMPL_H
#define EMULATOR_CAMERA_HAL_HWL_CAMERA_DEVICE_SESSION_HWL_IMPL_H

#include <set>

#include <camera_device_session_hwl.h>
#include "EmulatedCameraDeviceHWLImpl.h"
#include "EmulatedRequestProcessor.h"

namespace android {

using google_camera_hal::CameraDeviceHwl;
using google_camera_hal::CameraDeviceSessionHwl;
using google_camera_hal::HwlPipelineCallback;
using google_camera_hal::HwlPipelineRequest;
using google_camera_hal::StreamConfiguration;
using google_camera_hal::RequestTemplate;
using google_camera_hal::HalStream;

// Implementation of CameraDeviceSessionHwl interface
class EmulatedCameraDeviceSessionHwlImpl : public CameraDeviceSessionHwl {
 public:
  static std::unique_ptr<EmulatedCameraDeviceSessionHwlImpl> Create(uint32_t cameraId,
          std::unique_ptr<HalCameraMetadata> staticMeta);

  virtual ~EmulatedCameraDeviceSessionHwlImpl();

  static bool areCharacteristicsSupported(const HalCameraMetadata& characteristics);

  // Override functions in CameraDeviceSessionHwl
  status_t ConstructDefaultRequestSettings(
      RequestTemplate type,
      std::unique_ptr<HalCameraMetadata>* default_settings) override;

  status_t ConfigurePipeline(uint32_t physical_camera_id, HwlPipelineCallback hwl_pipeline_callback,
          const StreamConfiguration& request_config, const StreamConfiguration& overall_config,
          uint32_t* pipeline_id) override;

  status_t BuildPipelines() override;

  status_t GetConfiguredHalStream(uint32_t pipeline_id,
          std::vector<HalStream>* hal_streams) const override;

  void DestroyPipelines() override;

  status_t SubmitRequests(uint32_t frame_number,
          const std::vector<HwlPipelineRequest>& requests) override;

  status_t Flush() override;

  uint32_t GetCameraId() const override;

  std::vector<uint32_t> GetPhysicalCameraIds() const override;

  status_t GetCameraCharacteristics(
          std::unique_ptr<HalCameraMetadata>* characteristics) const override;

  status_t GetPhysicalCameraCharacteristics(uint32_t physical_camera_id,
          std::unique_ptr<HalCameraMetadata>* characteristics) const override;

  // End override functions in CameraDeviceSessionHwl

 private:

  status_t initialize(uint32_t cameraId, std::unique_ptr<HalCameraMetadata> staticMeta);

  // helper methods
  bool supportsCapability(uint8_t cap);

  EmulatedCameraDeviceSessionHwlImpl() :
      mMaxPipelineDepth(0) {}

  // Supported capabilities and features
  static const std::set<uint8_t> kSupportedCapabilites;
  std::set<uint8_t> mAvailableCapabilites;
  std::set<int32_t> mAvailableCharacteritics;
  std::set<int32_t> mAvailableResults;
  std::set<int32_t> mAvailableRequests;
  uint8_t mMaxPipelineDepth;

  // Protects the API entry points
  mutable std::mutex mAPIMutex;
  uint32_t mCameraId = 0;
  bool mErrorState = false;
  bool mPipelinesBuilt = false;
  std::unique_ptr<HalCameraMetadata> mStaticMetadata;
  std::vector<EmulatedPipeline> mPipelines;
  std::unique_ptr<EmulatedRequestProcessor> mRequestProcessor;
};

}  // namespace android

#endif
