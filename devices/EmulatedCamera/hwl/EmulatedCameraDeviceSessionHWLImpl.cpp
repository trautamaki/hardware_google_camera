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

#define LOG_TAG "EmulatedCameraDeviceSessionHwlImpl"
#define ATRACE_TAG ATRACE_TAG_CAMERA

#include <inttypes.h>

#include <hardware/gralloc.h>
#include <log/log.h>
#include <utils/Trace.h>

#include "EmulatedCameraDeviceSessionHWLImpl.h"

namespace android {

std::unique_ptr<EmulatedCameraDeviceSessionHwlImpl> EmulatedCameraDeviceSessionHwlImpl::Create(
        uint32_t cameraId, std::unique_ptr<HalCameraMetadata> staticMeta) {
    ATRACE_CALL();
    auto session = std::unique_ptr<EmulatedCameraDeviceSessionHwlImpl>(
            new EmulatedCameraDeviceSessionHwlImpl);
    if (session == nullptr) {
        ALOGE("%s: Creating EmulatedCameraDeviceSessionHwlImpl failed", __FUNCTION__);
        return nullptr;
    }

    status_t res = session->initialize(cameraId, std::move(staticMeta));
    if (res != OK) {
        ALOGE("%s: Initializing EmulatedCameraDeviceSessionHwlImpl failed: %s(%d)",
                __FUNCTION__, strerror(-res), res);
        return nullptr;
    }

    return session;
}

status_t EmulatedCameraDeviceSessionHwlImpl::initialize(uint32_t cameraId,
        std::unique_ptr<HalCameraMetadata> staticMeta) {
    //TODO: Iterate over static metadata and infer capabilities
    mCameraId = cameraId;
    mStaticMetadata = std::move(staticMeta);

    return OK;
}

EmulatedCameraDeviceSessionHwlImpl::~EmulatedCameraDeviceSessionHwlImpl() { }

status_t EmulatedCameraDeviceSessionHwlImpl::ConstructDefaultRequestSettings(
    RequestTemplate type, std::unique_ptr<HalCameraMetadata>* default_settings) {
    ATRACE_CALL();
    std::lock_guard<std::mutex> lock(mAPIMutex);
    if (default_settings == nullptr) {
        ALOGE("%s default_settings is nullptr", __FUNCTION__);
        return BAD_VALUE;
    }

    // TODO: Add in the rest depending on supported capabilities and eventually cache
    //       the results.
    (*default_settings) = HalCameraMetadata::Create(1, 10);
    uint8_t intent;
    switch (type) {
        case RequestTemplate::kManual:
            intent = ANDROID_CONTROL_CAPTURE_INTENT_MANUAL;
            break;
        case RequestTemplate::kPreview:
            intent = ANDROID_CONTROL_CAPTURE_INTENT_PREVIEW;
            break;
        case RequestTemplate::kStillCapture:
            intent = ANDROID_CONTROL_CAPTURE_INTENT_STILL_CAPTURE;
            break;
        case RequestTemplate::kVideoRecord:
            intent = ANDROID_CONTROL_CAPTURE_INTENT_VIDEO_RECORD;
            break;
        case RequestTemplate::kVideoSnapshot:
            intent = ANDROID_CONTROL_CAPTURE_INTENT_VIDEO_SNAPSHOT;
            break;
        case RequestTemplate::kZeroShutterLag:
            intent = ANDROID_CONTROL_CAPTURE_INTENT_ZERO_SHUTTER_LAG;
            break;
        default:
            ALOGE("%s Unsupported templated type: %d", __FUNCTION__, type);
            return BAD_VALUE;
    }

    (*default_settings)->Set(ANDROID_CONTROL_CAPTURE_INTENT, &intent, 1);

    return OK;
}

status_t EmulatedCameraDeviceSessionHwlImpl::ConfigurePipeline(uint32_t physical_camera_id,
        HwlPipelineCallback hwl_pipeline_callback, const StreamConfiguration& request_config,
        const StreamConfiguration& /*overall_config*/, uint32_t* pipeline_id) {
    ATRACE_CALL();
    std::lock_guard<std::mutex> lock(mAPIMutex);
    if (pipeline_id == nullptr) {
        ALOGE("%s pipeline_id is nullptr", __FUNCTION__);
        return BAD_VALUE;
    }

    if (mPipelinesBuilt) {
        ALOGE("%s Cannot configure pipelines after calling BuildPipelines()", __FUNCTION__);
        return ALREADY_EXISTS;
    }

    // TODO: check input pipeline

    *pipeline_id = mPipelines.size();
    EmulatedPipeline emulatedPipeline {
            .cb = hwl_pipeline_callback,
            .pipelineId = *pipeline_id,
            .physicalCameraId = physical_camera_id
    };

    emulatedPipeline.streams.reserve(request_config.streams.size());
    for (const auto& stream : request_config.streams) {
        HalStream halStream = {
                .id = stream.id,
                .override_format = stream.format,
                .producer_usage = GRALLOC_USAGE_SW_WRITE_OFTEN,
                .consumer_usage = 0,
                .max_buffers = 1, // TODO: calculate max buffers accordingly
                .override_data_space = stream.data_space,
                .is_physical_camera_stream = stream.is_physical_camera_stream,
                .physical_camera_id = stream.physical_camera_id,
        };
        emulatedPipeline.streams.push_back(halStream);
    }

    // TODO: configure pipeline

    mPipelines.push_back(emulatedPipeline);

    return OK;
}

status_t EmulatedCameraDeviceSessionHwlImpl::GetConfiguredHalStream(
    uint32_t pipeline_id, std::vector<HalStream>* hal_streams) const {
    ATRACE_CALL();
    ALOGE("%s: E", __func__);
    std::lock_guard<std::mutex> lock(mAPIMutex);
    if (hal_streams == nullptr) {
        ALOGE("%s hal_streams is nullptr", __FUNCTION__);
        return BAD_VALUE;
    }

    if (!mPipelinesBuilt) {
        ALOGE("%s No pipeline was built.", __FUNCTION__);
        return NO_INIT;
    }

    if (pipeline_id >= mPipelines.size()) {
        ALOGE("%s: Unknown pipeline ID: %u", __FUNCTION__, pipeline_id);
        return NAME_NOT_FOUND;
    }

    for (const auto& pipeline : mPipelines) {
        hal_streams->insert(hal_streams->begin(), pipeline.streams.begin(), pipeline.streams.end());
    }

    return OK;
}

status_t EmulatedCameraDeviceSessionHwlImpl::BuildPipelines() {
    ATRACE_CALL();
    std::lock_guard<std::mutex> lock(mAPIMutex);
    if (mPipelinesBuilt) {
        ALOGE("%s Pipelines have already been built!", __FUNCTION__);
        return ALREADY_EXISTS;
    } else if (mPipelines.size() == 0) {
        ALOGE("%s No pipelines have been configured yet!", __FUNCTION__);
        return NO_INIT;
    }

    //TODO: build pipelines

    mPipelinesBuilt = true;
    return OK;
}

void EmulatedCameraDeviceSessionHwlImpl::DestroyPipelines() {
    ATRACE_CALL();
    std::lock_guard<std::mutex> lock(mAPIMutex);
    if (!mPipelinesBuilt) {
        // Not an error - nothing to destroy
        ALOGV("%s nothing to destroy", __FUNCTION__);
        return;
    }

    mPipelinesBuilt = false;
    mPipelines.clear();
    // TODO
}

status_t EmulatedCameraDeviceSessionHwlImpl::SubmitRequests(uint32_t frame_number,
        const std::vector<HwlPipelineRequest>& requests) {
    ATRACE_CALL();
    std::lock_guard<std::mutex> lock(mAPIMutex);

    //TODO: Check input request

    if (mErrorState) {
        ALOGE("%s session is in error state and cannot process further requests", __FUNCTION__);
        return INVALID_OPERATION;
    }

    return mRequestProcessor->processPipelineRequests(frame_number, requests, mPipelines);
}

status_t EmulatedCameraDeviceSessionHwlImpl::Flush() {
    // TODO
    return OK;
}

uint32_t EmulatedCameraDeviceSessionHwlImpl::GetCameraId() const {
    return mCameraId;
}

std::vector<uint32_t> EmulatedCameraDeviceSessionHwlImpl::GetPhysicalCameraIds() const {
    // TODO: Logical camera support
    return std::vector<uint32_t> {};
}

status_t EmulatedCameraDeviceSessionHwlImpl::GetCameraCharacteristics(
    std::unique_ptr<HalCameraMetadata>* characteristics) const {
    ATRACE_CALL();
    if (characteristics == nullptr) {
        return BAD_VALUE;
    }

    (*characteristics) =
        HalCameraMetadata::Clone(mStaticMetadata.get());

    if (*characteristics == nullptr) {
        ALOGE("%s metadata clone failed", __FUNCTION__);
        return NO_MEMORY;
    }

    return OK;
}

status_t EmulatedCameraDeviceSessionHwlImpl::GetPhysicalCameraCharacteristics(
    uint32_t /*physical_camera_id*/,
    std::unique_ptr<HalCameraMetadata>* characteristics) const {
    ATRACE_CALL();
    if (characteristics == nullptr) {
        return BAD_VALUE;
    }

    // TODO: Add logical stream support
    return OK;
}

}  // namespace android
