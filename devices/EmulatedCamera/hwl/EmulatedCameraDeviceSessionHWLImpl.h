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
#include "EmulatedTorchState.h"
#include "utils/StreamConfigurationMap.h"

namespace android {

using google_camera_hal::CameraDeviceHwl;
using google_camera_hal::CameraDeviceSessionHwl;
using google_camera_hal::HwlPipelineCallback;
using google_camera_hal::HwlPipelineRequest;
using google_camera_hal::HwlSessionCallback;
using google_camera_hal::StreamConfiguration;
using google_camera_hal::RequestTemplate;
using google_camera_hal::SessionDataKey;
using google_camera_hal::HalStream;

// Implementation of CameraDeviceSessionHwl interface
class EmulatedCameraDeviceSessionHwlImpl : public CameraDeviceSessionHwl {
public:
    static std::unique_ptr<EmulatedCameraDeviceSessionHwlImpl> Create(uint32_t cameraId,
            std::unique_ptr<HalCameraMetadata> staticMeta,
            std::shared_ptr<EmulatedTorchState> torchState);

    virtual ~EmulatedCameraDeviceSessionHwlImpl();

    // Override functions in CameraDeviceSessionHwl
    status_t ConstructDefaultRequestSettings(
            RequestTemplate type,
            std::unique_ptr<HalCameraMetadata>* default_settings) override;

    status_t PrepareConfigureStreams(
            const StreamConfiguration& /*request_config*/) override { return OK; } // Noop for now

    status_t ConfigurePipeline(uint32_t physical_camera_id,
            HwlPipelineCallback hwl_pipeline_callback, const StreamConfiguration& request_config,
            const StreamConfiguration& overall_config, uint32_t* pipeline_id) override;

    status_t BuildPipelines() override;

    status_t PreparePipeline(uint32_t /*pipeline_id*/, uint32_t /*frame_number*/) override {
        return OK; } // Noop for now

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

    status_t SetSessionData(SessionDataKey /*key*/, void* /*value*/) override {
        return OK; } // Noop for now

    status_t GetSessionData(SessionDataKey /*key*/, void** /*value*/) const override {
        return OK; } // Noop for now

    void SetSessionCallback(const HwlSessionCallback& /*hwl_session_callback*/) override {}

    status_t FilterResultMetadata(HalCameraMetadata* /*metadata*/) const override {
        return OK; } // Noop for now
    // End override functions in CameraDeviceSessionHwl

private:

    status_t initialize(uint32_t cameraId, std::unique_ptr<HalCameraMetadata> staticMeta);

    EmulatedCameraDeviceSessionHwlImpl(
            std::shared_ptr<EmulatedTorchState> torchState) :
        mMaxPipelineDepth(0), mTorchState(torchState) {}

    uint8_t mMaxPipelineDepth;

    // Protects the API entry points
    mutable std::mutex mAPIMutex;
    uint32_t mCameraId = 0;
    bool mErrorState = false;
    bool mPipelinesBuilt = false;
    std::unique_ptr<HalCameraMetadata> mStaticMetadata;
    std::vector<EmulatedPipeline> mPipelines;
    std::unique_ptr<EmulatedRequestProcessor> mRequestProcessor;
    std::unique_ptr<StreamConfigurationMap> mStreamConigurationMap;
    SensorCharacteristics mSensorChars;
    std::shared_ptr<EmulatedTorchState> mTorchState;
};

}  // namespace android

#endif
