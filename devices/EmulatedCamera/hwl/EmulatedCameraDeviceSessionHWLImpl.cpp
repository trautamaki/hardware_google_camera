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
#include "HWLUtils.h"

namespace android {

const std::set<uint8_t> EmulatedCameraDeviceSessionHwlImpl::kSupportedCapabilites = {
    ANDROID_REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE,
    ANDROID_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR,
    ANDROID_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_POST_PROCESSING,
    ANDROID_REQUEST_AVAILABLE_CAPABILITIES_RAW,
    ANDROID_REQUEST_AVAILABLE_CAPABILITIES_READ_SENSOR_SETTINGS,
    ANDROID_REQUEST_AVAILABLE_CAPABILITIES_DEPTH_OUTPUT,
    //TODO: We should eventually support all capabilities
};

bool EmulatedCameraDeviceSessionHwlImpl::areCharacteristicsSupported(
        const HalCameraMetadata& characteristics) {

    camera_metadata_ro_entry_t entry;
    auto ret = characteristics.Get(ANDROID_REQUEST_PIPELINE_MAX_DEPTH, &entry);
    if ((ret == OK) && (entry.count == 1)) {
        if (entry.data.u8[0] == 0) {
            ALOGE("%s: Maximum request pipeline must have a non zero value!", __FUNCTION__);
            return false;
        }
    } else {
        ALOGE("%s: Maximum request pipeline request depth absent!", __FUNCTION__);
        return false;
    }

    ret = characteristics.Get(ANDROID_REQUEST_AVAILABLE_CAPABILITIES, &entry);
    if ((ret == OK) && (entry.count > 0)) {
        for (size_t i = 0; i < entry.count; i++) {
            if (kSupportedCapabilites.find(entry.data.u8[i]) == kSupportedCapabilites.end()) {
                ALOGE("%s: Capability: %u not supported", __FUNCTION__, entry.data.u8[i]);
                return false;
            }
        }
    } else {
        ALOGE("%s: No available capabilities!", __FUNCTION__);
        return false;
    }

    ret = characteristics.Get(ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS, &entry);
    if ((ret != OK) || (entry.count == 0)) {
        ALOGE("%s: No available characteristic keys!", __FUNCTION__);
        return false;
    }

    ret = characteristics.Get(ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS, &entry);
    if ((ret != OK) || (entry.count == 0)) {
        ALOGE("%s: No available characteristic keys!", __FUNCTION__);
        return false;
    }

    ret = characteristics.Get(ANDROID_REQUEST_AVAILABLE_RESULT_KEYS, &entry);
    if ((ret != OK) || (entry.count == 0)) {
        ALOGE("%s: No available result keys!", __FUNCTION__);
        return false;
    }

    ret = characteristics.Get(ANDROID_REQUEST_AVAILABLE_REQUEST_KEYS, &entry);
    if ((ret != OK) || (entry.count == 0)) {
        ALOGE("%s: No available request keys!", __FUNCTION__);
        return false;
    }

    return true;
}

std::unique_ptr<EmulatedCameraDeviceSessionHwlImpl> EmulatedCameraDeviceSessionHwlImpl::Create(
        uint32_t cameraId, std::unique_ptr<HalCameraMetadata> staticMeta) {
    ATRACE_CALL();
    if (staticMeta.get() == nullptr) {
        return nullptr;
    }

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
    if (!areCharacteristicsSupported(*staticMeta)) {
        return BAD_VALUE;
    }

    mCameraId = cameraId;
    mStaticMetadata = std::move(staticMeta);
    //TODO: Iterate over static metadata and infer capabilities plus features
    camera_metadata_ro_entry_t entry;
    mStaticMetadata->Get(ANDROID_REQUEST_PIPELINE_MAX_DEPTH, &entry);
    mMaxPipelineDepth = entry.data.u8[0];

    mStaticMetadata->Get(ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS, &entry);
    mAvailableCharacteritics.insert(entry.data.i32, entry.data.i32 + entry.count);
    mStaticMetadata->Get(ANDROID_REQUEST_AVAILABLE_RESULT_KEYS, &entry);
    mAvailableResults.insert(entry.data.i32, entry.data.i32 + entry.count);
    mStaticMetadata->Get(ANDROID_REQUEST_AVAILABLE_REQUEST_KEYS, &entry);
    mAvailableRequests.insert(entry.data.i32, entry.data.i32 + entry.count);

    mStaticMetadata->Get(ANDROID_REQUEST_AVAILABLE_CAPABILITIES, &entry);
    mAvailableCapabilites.insert(entry.data.u8, entry.data.u8 + entry.count);
    EmulatedSensor::SensorCharacteristics sensorCharacteristics;
    auto ret = getSensorCharacteristics(mStaticMetadata.get(), &sensorCharacteristics);
    if (ret != OK) {
        ALOGE("%s: Unable to extract sensor characteristics %s (%d)", __FUNCTION__, strerror(-ret),
                ret);
        return ret;
    }

    sp<EmulatedSensor> emulatedSensor = new EmulatedSensor();
    ret = emulatedSensor->startUp(sensorCharacteristics);
    if (ret != OK) {
        ALOGE("%s: Failed on sensor start up %s (%d)", __FUNCTION__, strerror(-ret), ret);
        return ret;
    }

    mRequestProcessor = std::make_unique<EmulatedRequestProcessor> (mMaxPipelineDepth,
            emulatedSensor);

    return OK;
}

bool EmulatedCameraDeviceSessionHwlImpl::supportsCapability(uint8_t cap) {
    return mAvailableCapabilites.find(cap) != mAvailableCapabilites.end();
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
        emulatedPipeline.streams.push_back({{
                .id = stream.id,
                .override_format = stream.format,
                .producer_usage = GRALLOC_USAGE_SW_WRITE_OFTEN,
                .consumer_usage = 0,
                .max_buffers = mMaxPipelineDepth,
                .override_data_space = stream.data_space,
                .is_physical_camera_stream = stream.is_physical_camera_stream,
                .physical_camera_id = stream.physical_camera_id
            },
            .width = stream.width,
            .height = stream.height,
            .bufferSize = stream.buffer_size});
    }

    // TODO: configure pipeline

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
