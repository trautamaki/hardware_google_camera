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

//#define LOG_NDEBUG 0
#define LOG_TAG "EmulatedCameraDeviceHwlImpl"
#include <log/log.h>

#include "camera_common.h"
#include "EmulatedCameraDeviceHWLImpl.h"

namespace android {

std::unique_ptr<CameraDeviceHwl> EmulatedCameraDeviceHwlImpl::Create(
        uint32_t cameraId, std::unique_ptr<HalCameraMetadata> staticMeta) {
    auto device = std::unique_ptr<EmulatedCameraDeviceHwlImpl>(new EmulatedCameraDeviceHwlImpl(
                cameraId, std::move(staticMeta)));

    if (device == nullptr) {
        ALOGE("%s: Creating EmulatedCameraDeviceHwlImpl failed.", __FUNCTION__);
        return nullptr;
    }

    status_t res = device->initialize();
    if (res != OK) {
        ALOGE("%s: Initializing EmulatedCameraDeviceHwlImpl failed: %s (%d).", __FUNCTION__,
                strerror(-res), res);
        return nullptr;
    }

    ALOGI("%s: Created EmulatedCameraDeviceHwlImpl for camera %u", __FUNCTION__,
            device->mCameraId);

    return device;
}

EmulatedCameraDeviceHwlImpl::EmulatedCameraDeviceHwlImpl(uint32_t cameraId,
        std::unique_ptr<HalCameraMetadata> staticMeta) :
    mCameraId(cameraId), mStaticMetadata(std::move(staticMeta)) { }

uint32_t EmulatedCameraDeviceHwlImpl::GetCameraId() const {
  return mCameraId;
}

status_t EmulatedCameraDeviceHwlImpl::initialize() {
    // TODO: initialization
    return OK;
}

status_t EmulatedCameraDeviceHwlImpl::GetResourceCost(CameraResourceCost* cost) const {
    // TODO: remove hardcode
    cost->resource_cost = 100;

    return OK;
}

status_t EmulatedCameraDeviceHwlImpl::GetCameraCharacteristics(
    std::unique_ptr<HalCameraMetadata>* characteristics) const {
    if (characteristics == nullptr) {
        return BAD_VALUE;
    }

    *characteristics = HalCameraMetadata::Clone(mStaticMetadata.get());

    return OK;
}

status_t EmulatedCameraDeviceHwlImpl::GetPhysicalCameraCharacteristics(
    uint32_t /*physical_camera_id*/,
    std::unique_ptr<HalCameraMetadata>* /*characteristics*/) const {

    // TODO: impl
    return OK;
}

status_t EmulatedCameraDeviceHwlImpl::SetTorchMode(TorchMode mode) {
    if (mode != TorchMode::kOn && mode != TorchMode::kOff) {
        ALOGE("%s: CameraId %u set invalid mode %u.", __FUNCTION__, mCameraId, mode);
        return INVALID_OPERATION;
    }

    // TODO: impl
    return OK;
}

status_t EmulatedCameraDeviceHwlImpl::DumpState(int /*fd*/) {
    return OK;
}

status_t EmulatedCameraDeviceHwlImpl::CreateCameraDeviceSessionHwl(
    CameraBufferAllocatorHwl* /*camera_allocator_hwl*/,
    std::unique_ptr<CameraDeviceSessionHwl>* session) {
    if (session == nullptr) {
        ALOGE("%s: session is nullptr.", __FUNCTION__);
        return BAD_VALUE;
    }

    // TODO: impl
    return OK;
}

}  // namespace android
