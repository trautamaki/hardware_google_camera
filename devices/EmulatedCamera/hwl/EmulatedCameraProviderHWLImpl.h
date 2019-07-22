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

#ifndef EMULATOR_CAMERA_HAL_HWL_CAMERA_PROVIDER_HWL_H
#define EMULATOR_CAMERA_HAL_HWL_CAMERA_PROVIDER_HWL_H

#include <hal_types.h>
#include <json/json.h>
#include <json/reader.h>

#include <camera_provider_hwl.h>

namespace android {

using google_camera_hal::CameraProviderHwl;
using google_camera_hal::CameraDeviceHwl;
using google_camera_hal::HwlCameraProviderCallback;
using google_camera_hal::VendorTagSection;
using google_camera_hal::CameraBufferAllocatorHwl;
using google_camera_hal::HalCameraMetadata;

class EmulatedCameraProviderHwlImpl : public CameraProviderHwl {
public:
    // Return a unique pointer to EmulatedCameraProviderHwlImpl. Calling Create()
    // again before the previous one is destroyed will fail.
    static std::unique_ptr<EmulatedCameraProviderHwlImpl> Create();

    virtual ~EmulatedCameraProviderHwlImpl() = default;

    // Override functions in CameraProviderHwl.
    status_t SetCallback(const HwlCameraProviderCallback& callback) override;

    status_t GetVendorTags(
            std::vector<VendorTagSection>* vendor_tag_sections) override;

    status_t GetVisibleCameraIds(std::vector<std::uint32_t>* camera_ids) override;

    bool IsSetTorchModeSupported() override { return true; }

    status_t CreateCameraDeviceHwl(
            uint32_t cameraId,
            std::unique_ptr<CameraDeviceHwl>* camera_device_hwl) override;

    status_t CreateBufferAllocatorHwl(std::unique_ptr<CameraBufferAllocatorHwl>*
            camera_buffer_allocator_hwl) override;
    // End of override functions in CameraProviderHwl.

private:
    status_t initialize();
    status_t parseCharacteristics(const Json::Value& root);
    status_t getTagFromName(const char *name, uint32_t *tag);

    static const char* kConfigurationFileLocation[];
    static const char* kCameraDefinitionsKey;

    std::vector<std::unique_ptr<HalCameraMetadata>> mStaticMetadata;
};

extern "C" CameraProviderHwl* CreateCameraProviderHwl() {
    auto provider = EmulatedCameraProviderHwlImpl::Create();
    return provider.release();
}

}  // namespace android

#endif  // EMULATOR_CAMERA_HAL_HWL_CAMERA_PROVIDER_HWL_H
