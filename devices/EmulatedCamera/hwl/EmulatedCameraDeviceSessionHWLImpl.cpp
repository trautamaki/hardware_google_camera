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
#include "EmulatedSensor.h"
#include "utils/HWLUtils.h"

namespace android {

std::unique_ptr<EmulatedCameraDeviceSessionHwlImpl> EmulatedCameraDeviceSessionHwlImpl::Create(
        uint32_t cameraId, std::unique_ptr<HalCameraMetadata> staticMeta,
        std::shared_ptr<EmulatedTorchState> torchState) {
    ATRACE_CALL();
    if (staticMeta.get() == nullptr) {
        return nullptr;
    }

    auto session = std::unique_ptr<EmulatedCameraDeviceSessionHwlImpl>(
            new EmulatedCameraDeviceSessionHwlImpl(torchState));
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
    mCameraId = cameraId;
    mStaticMetadata = std::move(staticMeta);
    mStreamConigurationMap = std::make_unique<StreamConfigurationMap>(*mStaticMetadata);
    camera_metadata_ro_entry_t entry;
    auto ret = mStaticMetadata->Get(ANDROID_REQUEST_PIPELINE_MAX_DEPTH, &entry);
    if (ret != OK) {
        ALOGE("%s: Unable to extract ANDROID_REQUEST_PIPELINE_MAX_DEPTH, %s (%d)", __FUNCTION__,
                strerror(-ret), ret);
        return ret;
    }

    mMaxPipelineDepth = entry.data.u8[0];

    ret = getSensorCharacteristics(mStaticMetadata.get(), &mSensorChars);
    if (ret != OK) {
        ALOGE("%s: Unable to extract sensor characteristics %s (%d)", __FUNCTION__, strerror(-ret),
                ret);
        return ret;
    }

    sp<EmulatedSensor> emulatedSensor = new EmulatedSensor();
    ret = emulatedSensor->startUp(mSensorChars);
    if (ret != OK) {
        ALOGE("%s: Failed on sensor start up %s (%d)", __FUNCTION__, strerror(-ret), ret);
        return ret;
    }

    mRequestProcessor = std::make_unique<EmulatedRequestProcessor> (mCameraId, emulatedSensor);

    return mRequestProcessor->initialize(HalCameraMetadata::Clone(mStaticMetadata.get()));
}

EmulatedCameraDeviceSessionHwlImpl::~EmulatedCameraDeviceSessionHwlImpl() {
    if (mTorchState.get() != nullptr) {
        mTorchState->releaseFlashHw();
    }
}

status_t EmulatedCameraDeviceSessionHwlImpl::ConstructDefaultRequestSettings(
    RequestTemplate type, std::unique_ptr<HalCameraMetadata>* default_settings) {
    ATRACE_CALL();
    std::lock_guard<std::mutex> lock(mAPIMutex);

    return mRequestProcessor->getDefaultRequest(type, default_settings);
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

    if (!EmulatedSensor::isStreamCombinationSupported(request_config, *mStreamConigurationMap,
                mSensorChars)) {
        ALOGE("%s: Stream combination not supported!", __FUNCTION__);
        return BAD_VALUE;
    }

    *pipeline_id = mPipelines.size();
    EmulatedPipeline emulatedPipeline {
            .cb = hwl_pipeline_callback,
            .pipelineId = *pipeline_id,
            .physicalCameraId = physical_camera_id
    };

    emulatedPipeline.streams.reserve(request_config.streams.size());
    for (const auto& stream : request_config.streams) {
        emulatedPipeline.streams.emplace(std::make_pair<uint32_t, EmulatedStream>(stream.id, {{
                .id = stream.id,
                .override_format = EmulatedSensor::overrideFormat(stream.format),
                .producer_usage = GRALLOC_USAGE_SW_WRITE_OFTEN,
                .consumer_usage = 0,
                .max_buffers = mMaxPipelineDepth,
                .override_data_space = stream.data_space,
                .is_physical_camera_stream = stream.is_physical_camera_stream,
                .physical_camera_id = stream.physical_camera_id},
            .width = stream.width,
            .height = stream.height,
            .bufferSize = stream.buffer_size}));
    }

    mPipelines.push_back(emulatedPipeline);

    return OK;
}

status_t EmulatedCameraDeviceSessionHwlImpl::GetConfiguredHalStream(
    uint32_t pipeline_id, std::vector<HalStream>* hal_streams) const {
    ATRACE_CALL();
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

    const auto& streams = mPipelines[pipeline_id].streams;
    hal_streams->reserve(streams.size());
    for (const auto& it : streams) {
        hal_streams->push_back(it.second);
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
    ATRACE_CALL();
    std::lock_guard<std::mutex> lock(mAPIMutex);
    return mRequestProcessor->flush();
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
