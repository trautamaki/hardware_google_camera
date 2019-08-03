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

#ifndef EMULATOR_CAMERA_HAL_HWL_CAMERA_DEVICE_HWL_H
#define EMULATOR_CAMERA_HAL_HWL_CAMERA_DEVICE_HWL_H

#include <camera_device_hwl.h>
#include "EmulatedSensor.h"
#include "EmulatedTorchState.h"
#include <hal_types.h>
#include "utils/StreamConfigurationMap.h"

namespace android {

using google_camera_hal::CameraBufferAllocatorHwl;
using google_camera_hal::CameraDeviceHwl;
using google_camera_hal::CameraDeviceSessionHwl;
using google_camera_hal::CameraResourceCost;
using google_camera_hal::HalCameraMetadata;
using google_camera_hal::StreamConfiguration;
using google_camera_hal::TorchMode;

class EmulatedCameraDeviceHwlImpl : public CameraDeviceHwl {
public:
    static std::unique_ptr<CameraDeviceHwl> Create(uint32_t cameraId,
            std::unique_ptr<HalCameraMetadata> staticMeta,
            std::shared_ptr<EmulatedTorchState> torchState);

    virtual ~EmulatedCameraDeviceHwlImpl() = default;

    // Override functions in CameraDeviceHwl.
    uint32_t GetCameraId() const override;

    status_t GetResourceCost(CameraResourceCost* cost) const override;

    status_t GetCameraCharacteristics(
            std::unique_ptr<HalCameraMetadata>* characteristics) const override;

    status_t GetPhysicalCameraCharacteristics(
            uint32_t physical_camera_id,
            std::unique_ptr<HalCameraMetadata>* characteristics) const override;

    status_t SetTorchMode(TorchMode mode) override;

    status_t DumpState(int fd) override;

    status_t CreateCameraDeviceSessionHwl(
            CameraBufferAllocatorHwl* camera_allocator_hwl,
            std::unique_ptr<CameraDeviceSessionHwl>* session) override;

    bool IsStreamCombinationSupported(const StreamConfiguration& stream_config) override;

    // End of override functions in CameraDeviceHwl.

private:
    const uint32_t mCameraId = 0;

    EmulatedCameraDeviceHwlImpl(uint32_t camera_id,
            std::unique_ptr<HalCameraMetadata> staticMeta,
            std::shared_ptr<EmulatedTorchState> torchState);

    status_t initialize();

    std::unique_ptr<HalCameraMetadata> mStaticMetadata;
    std::unique_ptr<StreamConfigurationMap> mStreamConigurationMap;
    std::shared_ptr<EmulatedTorchState> mTorchState;
    SensorCharacteristics mSensorChars;
};

}  // namespace android

#endif  // EMULATOR_CAMERA_HAL_HWL_CAMERA_DEVICE_HWL_H
