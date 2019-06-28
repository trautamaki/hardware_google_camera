/*
 * Copyright (C) 2019 The Android Open Source Project
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

#define LOG_TAG "EmulatedRequestProcessor"
#define ATRACE_TAG ATRACE_TAG_CAMERA

#include "EmulatedRequestProcessor.h"
#include <HandleImporter.h>
#include <log/log.h>
#include <sync/sync.h>
#include <utils/Timers.h>
#include <utils/Trace.h>

namespace android {

using android::hardware::camera::common::V1_0::helper::HandleImporter;
using google_camera_hal::ErrorCode;
using google_camera_hal::HwlPipelineResult;
using google_camera_hal::NotifyMessage;
using google_camera_hal::MessageType;

const std::set<uint8_t> EmulatedRequestProcessor::kSupportedCapabilites = {
    ANDROID_REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE,
    ANDROID_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR,
    ANDROID_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_POST_PROCESSING,
    ANDROID_REQUEST_AVAILABLE_CAPABILITIES_RAW,
    ANDROID_REQUEST_AVAILABLE_CAPABILITIES_READ_SENSOR_SETTINGS,
    //TODO: Support more capabilities out-of-the box
};

const std::set<uint8_t> EmulatedRequestProcessor::kSupportedHWLevels = {
    ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_LIMITED,
    ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_FULL,
    //TODO: Support more hw levels out-of-the box
};

EmulatedRequestProcessor::EmulatedRequestProcessor(uint32_t cameraId, uint8_t maxPipelineDepth,
        sp<EmulatedSensor> sensor) :
    mMaxPipelineDepth(maxPipelineDepth),
    mCameraId(cameraId),
    mSensor(sensor) {
    ATRACE_CALL();
    mRequestThread = std::thread([this] { this->requestProcessorLoop(); });
}

EmulatedRequestProcessor::~EmulatedRequestProcessor() {
    ATRACE_CALL();
    mProcessorDone = true;
    mRequestThread.join();

    auto ret = mSensor->shutDown();
    if (ret != OK) {
        ALOGE("%s: Failed during sensor shutdown %s (%d)", __FUNCTION__, strerror(-ret), ret);
    }
}

status_t EmulatedRequestProcessor::processPipelineRequests(uint32_t frameNumber,
        const std::vector<HwlPipelineRequest>& requests,
        const std::vector<EmulatedPipeline>& pipelines) {
    ATRACE_CALL();

    std::unique_lock<std::mutex> lock(mProcessMutex);

    for (const auto& request : requests) {
        if (request.pipeline_id >= pipelines.size()) {
            ALOGE("%s: Pipeline request with invalid pipeline id: %u", __FUNCTION__,
                    request.pipeline_id);
            return BAD_VALUE;
        }

        while (mPendingRequests.size() >= mMaxPipelineDepth) {
            auto result = mRequestCondition.wait_for(lock,
                    std::chrono::nanoseconds(EmulatedSensor::kSupportedFrameDurationRange[1]));
            if (result == std::cv_status::timeout) {
                ALOGE("%s Timed out waiting for a pending request slot", __FUNCTION__);
                return TIMED_OUT;
            }
        }

        const auto& streams = pipelines[request.pipeline_id].streams;
        auto sensorBuffers = std::make_unique<Buffers>();
        sensorBuffers->reserve(request.output_buffers.size());
        for (const auto& outputBuffer : request.output_buffers) {
            auto sensorBuffer = createSensorBuffer(frameNumber, streams.at(outputBuffer.stream_id),
                    request, pipelines[request.pipeline_id].cb, outputBuffer);
            if (sensorBuffer.get() != nullptr) {
                sensorBuffers->push_back(std::move(sensorBuffer));
            }
        }

        mPendingRequests.push({
                .settings = HalCameraMetadata::Clone(request.settings.get()),
                .inputBuffers = request.input_buffers,
                .outputBuffers = std::move(sensorBuffers)});
    }

    return OK;
}

void EmulatedRequestProcessor::notifyFailedRequest(const PendingRequest& request) {
    if ((request.outputBuffers.get() == nullptr) || (request.outputBuffers->empty())) {
        return;
    }

    if (request.outputBuffers->at(0)->callback.notify != nullptr) {
        NotifyMessage msg = {
            .type = MessageType::kError,
            .message.error = {
                .frame_number = request.outputBuffers->at(0)->frameNumber,
                .error_stream_id = -1,
                .error_code = ErrorCode::kErrorRequest
            }
        };
        request.outputBuffers->at(0)->callback.notify(
                request.outputBuffers->at(0)->pipelineId, msg);
    }

    for (const auto& buffer : (*request.outputBuffers)) {
        buffer->streamBuffer.status = BufferStatus::kError;
    }
}

status_t EmulatedRequestProcessor::flush() {
    std::lock_guard<std::mutex> lock(mProcessMutex);
    // First flush in-flight requests
    auto ret = mSensor->flush();

    // Then the rest of the pending requests
    while (!mPendingRequests.empty()) {
        const auto& request = mPendingRequests.front();
        notifyFailedRequest(request);
        mPendingRequests.pop();
    }

    return ret;
}

status_t EmulatedRequestProcessor::getBufferSizeAndStride(const EmulatedStream& stream,
        uint32_t *size /*out*/, uint32_t *stride /*out*/) {
    if (size == nullptr) {
        return BAD_VALUE;
    }

    switch (stream.override_format) {
        case HAL_PIXEL_FORMAT_RGB_888:
            *stride = stream.width * 3;
            *size = (*stride) * stream.width;
            break;
        case HAL_PIXEL_FORMAT_RGBA_8888:
            *stride = stream.width * 4;;
            *size = (*stride) * stream.width;
            break;
        case HAL_PIXEL_FORMAT_Y16:
            if (stream.override_data_space == HAL_DATASPACE_DEPTH) {
                *stride = alignTo(alignTo(stream.width, 2) * 2, 16);
                *size = (*stride) * alignTo(stream.height, 2);
            } else {
                return BAD_VALUE;
            }
            break;
        case HAL_PIXEL_FORMAT_BLOB:
            if (stream.override_data_space == HAL_DATASPACE_V0_JFIF) {
                *size = stream.bufferSize;
                *stride = *size;
            } else {
                return BAD_VALUE;
            }
            break;
        case HAL_PIXEL_FORMAT_RAW16:
            *stride = stream.width * 2;
            *size = (*stride) * stream.width;
            break;
        default:
            return BAD_VALUE;
    }

    return OK;
}

status_t EmulatedRequestProcessor::lockSensorBuffer(const EmulatedStream& stream,
        HandleImporter& importer /*in*/, buffer_handle_t buffer,
        SensorBuffer *sensorBuffer /*out*/) {
    if (sensorBuffer == nullptr) {
        return BAD_VALUE;
    }

    auto width = static_cast<int32_t> (stream.width);
    auto height = static_cast<int32_t> (stream.height);
    if (stream.override_format == HAL_PIXEL_FORMAT_YCBCR_420_888) {
        IMapper::Rect mapRect = {0, 0, width, height};
        auto yuvLayout = importer.lockYCbCr(buffer, stream.producer_usage, mapRect);
        if ((yuvLayout.y != nullptr) && (yuvLayout.cb != nullptr) &&
                (yuvLayout.cr != nullptr)) {
            sensorBuffer->plane.imgYCrCb.imgY = static_cast<uint8_t *> (yuvLayout.y);
            sensorBuffer->plane.imgYCrCb.imgCb = static_cast<uint8_t *> (yuvLayout.cb);
            sensorBuffer->plane.imgYCrCb.imgCr = static_cast<uint8_t *> (yuvLayout.cr);
            sensorBuffer->plane.imgYCrCb.yStride = yuvLayout.yStride;
            sensorBuffer->plane.imgYCrCb.CbCrStride = yuvLayout.cStride;
            sensorBuffer->plane.imgYCrCb.CbCrStep = yuvLayout.chromaStep;
        } else {
            ALOGE("%s: Failed to lock output buffer!", __FUNCTION__);
            return BAD_VALUE;
        }
    } else {
        uint32_t bufferSize = 0, stride = 0;
        auto ret = getBufferSizeAndStride(stream, &bufferSize, &stride);
        if (ret == OK) {
            sensorBuffer->plane.img.img = static_cast<uint8_t *> (importer.lock(buffer,
                        stream.producer_usage, bufferSize));
            if (sensorBuffer->plane.img.img != nullptr) {
                sensorBuffer->plane.img.stride = stride;
                sensorBuffer->plane.img.bufferSize = bufferSize;
            } else {
                ALOGE("%s: Failed to lock output buffer!", __FUNCTION__);
                return ret;
            }
        } else {
            ALOGE("%s: Unsupported pixel format: 0x%x", __FUNCTION__,
                    stream.override_format);
            return BAD_VALUE;
        }
    }

    return OK;
}

std::unique_ptr<SensorBuffer> EmulatedRequestProcessor::createSensorBuffer(uint32_t frameNumber,
        const EmulatedStream& stream, const HwlPipelineRequest& request,
        HwlPipelineCallback callback, StreamBuffer streamBuffer) {
    auto buffer = std::make_unique<SensorBuffer>();

    buffer->width = stream.width;
    buffer->height = stream.height;
    buffer->format = stream.override_format;
    buffer->dataSpace = stream.override_data_space;
    buffer->streamBuffer = streamBuffer;
    buffer->pipelineId = request.pipeline_id;
    buffer->callback = callback;
    buffer->frameNumber = frameNumber;
    buffer->cameraId = mCameraId;

    auto ret = lockSensorBuffer(stream, buffer->importer, streamBuffer.buffer, buffer.get());
    if (ret != OK) {
        buffer->streamBuffer.status = BufferStatus::kError;
        buffer.release();
        buffer = nullptr;
    }

    if (streamBuffer.acquire_fence != nullptr) {
        auto fenceStatus = buffer->importer.importFence(streamBuffer.acquire_fence,
                buffer->acquireFenceFd);
        if (!fenceStatus) {
            ALOGE("%s: Failed importing acquire fence!", __FUNCTION__);
            buffer->streamBuffer.status = BufferStatus::kError;
            buffer.release();
            buffer = nullptr;
        }
    }

    return buffer;
}

std::unique_ptr<Buffers> EmulatedRequestProcessor::initializeOutputBuffers(
        const PendingRequest& request) {
    auto outputBuffers = std::make_unique<Buffers>();

    outputBuffers->reserve(request.outputBuffers->size());
    auto outputBuffer = request.outputBuffers->begin();
    while (outputBuffer != request.outputBuffers->end()) {
        if((*outputBuffer)->acquireFenceFd >= 0) {
            auto ret = sync_wait((*outputBuffer)->acquireFenceFd,
                    ns2ms(EmulatedSensor::kSupportedFrameDurationRange[1]));
            if (ret != OK) {
                ALOGE("%s: Fence sync failed: %s, (%d)", __FUNCTION__, strerror(-ret),
                        ret);
                (*outputBuffer)->streamBuffer.status = BufferStatus::kError;
            }
        }

        if ((*outputBuffer)->streamBuffer.status == BufferStatus::kOk) {
            outputBuffers->push_back(std::move(*outputBuffer));
        }

        outputBuffer = request.outputBuffers->erase(outputBuffer);
    }

    return outputBuffers;
}

status_t EmulatedRequestProcessor::initializeSensorSettingsLocked(const PendingRequest& request,
        EmulatedSensor::SensorSettings *settings/*out*/) {
    if (settings == nullptr) {
        return BAD_VALUE;
    }

    if (request.settings.get() == nullptr) {
        // No settings, apply settings for last request
        settings->exposureTime = mSensorExposureTime;
        settings->frameDuration = mSensorFrameDuration;
        settings->gain = mSensorSensitivity;

        return OK;
    }

    camera_metadata_ro_entry_t entry;
    auto ret = request.settings->Get(ANDROID_CONTROL_MODE, &entry);
    if ((ret == OK) && (entry.count == 1)) {
        if (mAvailableControlModes.find(entry.data.u8[0]) != mAvailableControlModes.end()) {
            mControlMode = entry.data.u8[0];
        } else {
            ALOGE("%s: Control mode: %d not supported!", __FUNCTION__, entry.data.u8[0]);
            return BAD_VALUE;
        }
    }

    ret = request.settings->Get(ANDROID_CONTROL_AE_MODE, &entry);
    if ((ret == OK) && (entry.count == 1)) {
        if (mAvailableAEModes.find(entry.data.u8[0]) != mAvailableAEModes.end()) {
            mAEMode = entry.data.u8[0];
        } else {
            ALOGE("%s: AE mode: %d not supported!", __FUNCTION__, entry.data.u8[0]);
            return BAD_VALUE;
        }
    }

    if (((mAEMode == ANDROID_CONTROL_AE_MODE_OFF) || (mControlMode == ANDROID_CONTROL_MODE_OFF)) &&
            supportsCapability(ANDROID_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR)) {

        ret = request.settings->Get(ANDROID_SENSOR_EXPOSURE_TIME, &entry);
        if ((ret == OK) && (entry.count == 1)) {
            if ((entry.data.i64[0] >= mSensorExposureTimeRange.first) &&
                    (entry.data.i64[0] <= mSensorExposureTimeRange.second)) {
                mSensorExposureTime = entry.data.i64[0];
            } else {
                ALOGE("%s: Sensor exposure time: %" PRId64 " not within supported range[%"
                        PRId64 ", %" PRId64"]", __FUNCTION__, entry.data.i64[0],
                            mSensorExposureTimeRange.first, mSensorExposureTimeRange.second);
                return BAD_VALUE;
            }
        }

        ret = request.settings->Get(ANDROID_SENSOR_FRAME_DURATION, &entry);
        if ((ret == OK) && (entry.count == 1)) {
            if ((entry.data.i64[0] >= EmulatedSensor::kSupportedFrameDurationRange[0]) &&
                    (entry.data.i64[0] <= mSensorMaxFrameDuration)) {
                mSensorFrameDuration = entry.data.i64[0];
            } else {
                ALOGE("%s: Sensor frame duration : %" PRId64 " not within supported range[%"
                        PRId64 ", %" PRId64"]", __FUNCTION__, entry.data.i64[0],
                            EmulatedSensor::kSupportedFrameDurationRange[0],
                            mSensorMaxFrameDuration);
                return BAD_VALUE;
            }
        }

        ret = request.settings->Get(ANDROID_SENSOR_SENSITIVITY, &entry);
        if ((ret == OK) && (entry.count == 1)) {
            if ((entry.data.i32[0] >= mSensorSensitivityRange.first) &&
                    (entry.data.i32[0] <= mSensorSensitivityRange.second)) {
                mSensorSensitivity = entry.data.i32[0];
            } else {
                ALOGE("%s: Sensor sensitivity: %d not within supported range[%d, %d]", __FUNCTION__,
                        entry.data.i32[0], mSensorSensitivityRange.first,
                        mSensorSensitivityRange.second);
                return BAD_VALUE;
            }
        }
    } else {
        //TODO run fake AE
    }

    settings->exposureTime = mSensorExposureTime;
    settings->frameDuration = mSensorFrameDuration;
    settings->gain = mSensorSensitivity;

    return OK;
}

std::unique_ptr<HwlPipelineResult> EmulatedRequestProcessor::initializeResultLocked(
        const PendingRequest& request, uint32_t pipelineId, uint32_t frameNumber) {
    auto result = std::make_unique<HwlPipelineResult>();
    result->camera_id = mCameraId;
    result->pipeline_id = pipelineId;
    result->frame_number = frameNumber;
    result->result_metadata = request.settings != nullptr ?
            HalCameraMetadata::Clone(request.settings.get()) : HalCameraMetadata::Create(1, 10);
    result->result_metadata->Set(ANDROID_REQUEST_PIPELINE_DEPTH, &mMaxPipelineDepth, 1);
    result->result_metadata->Set(ANDROID_CONTROL_MODE, &mControlMode, 1);
    result->result_metadata->Set(ANDROID_CONTROL_AE_MODE, &mAEMode, 1);
    if (supportsCapability(ANDROID_REQUEST_AVAILABLE_CAPABILITIES_READ_SENSOR_SETTINGS)) {
        result->result_metadata->Set(ANDROID_SENSOR_EXPOSURE_TIME, &mSensorExposureTime, 1);
        result->result_metadata->Set(ANDROID_SENSOR_FRAME_DURATION, &mSensorFrameDuration, 1);
        result->result_metadata->Set(ANDROID_SENSOR_SENSITIVITY, &mSensorSensitivity, 1);
    }
    result->input_buffers = request.inputBuffers;
    result->partial_result = 1; //TODO: Change in case of partial result support

    return result;
}

void EmulatedRequestProcessor::requestProcessorLoop() {
    ATRACE_CALL();

    while (!mProcessorDone) {
        {
            std::lock_guard<std::mutex> lock(mProcessMutex);
            if (!mPendingRequests.empty()) {
                const auto& request = mPendingRequests.front();

                EmulatedSensor::SensorSettings settings;
                auto ret = initializeSensorSettingsLocked(request, &settings);
                auto outputBuffers = initializeOutputBuffers(request);
                if (!outputBuffers->empty() && (ret == OK)) {
                    auto result = initializeResultLocked(request, outputBuffers->at(0)->pipelineId,
                            outputBuffers->at(0)->frameNumber);
                    mSensor->setCurrentRequest(settings, std::move(result),
                            std::move(outputBuffers));
                } else {
                    //TODO: Notify about failed result
                }

                mPendingRequests.pop();
                mRequestCondition.notify_one();
            }
        }

        mSensor->waitForVSync(EmulatedSensor::kSupportedFrameDurationRange[1]);
    }
}

bool EmulatedRequestProcessor::supportsCapability(uint8_t cap) {
    return mAvailableCapabilites.find(cap) != mAvailableCapabilites.end();
}

bool EmulatedRequestProcessor::isBackwardCompatible() {
    return supportsCapability(ANDROID_REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE);
}

template<typename T>
T getClosestValue(T val, T min, T max) {
    if ((min > max) || ((val >= min) && (val <= max))) {
        return val;
    } else if (val > max) {
        return max;
    } else {
        return min;
    }
}

status_t EmulatedRequestProcessor::initializeSensorDefaults() {
    bool manualSensor = supportsCapability(ANDROID_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR);
    camera_metadata_ro_entry_t entry;
    auto ret = mStaticMetadata->Get(ANDROID_SENSOR_INFO_SENSITIVITY_RANGE, &entry);
    if ((ret == OK) && (entry.count == 2)) {
        mSensorSensitivityRange = std::make_pair(entry.data.i32[0], entry.data.i32[1]);
    } else if (!manualSensor) {
        mSensorSensitivityRange = std::make_pair(EmulatedSensor::kSupportedSensitivityRange[0],
                EmulatedSensor::kSupportedSensitivityRange[1]);
    } else {
        ALOGE("%s: Manual sensor devices must advertise sensor sensitivity range!", __FUNCTION__);
        return BAD_VALUE;
    }

    ret = mStaticMetadata->Get(ANDROID_SENSOR_INFO_EXPOSURE_TIME_RANGE, &entry);
    if ((ret == OK) && (entry.count == 2)) {
        mSensorExposureTimeRange = std::make_pair(entry.data.i64[0], entry.data.i64[1]);
    } else if (!manualSensor) {
        mSensorExposureTimeRange = std::make_pair(EmulatedSensor::kSupportedExposureTimeRange[0],
                EmulatedSensor::kSupportedExposureTimeRange[1]);
    } else {
        ALOGE("%s: Manual sensor devices must advertise sensor exposure time range!", __FUNCTION__);
        return BAD_VALUE;
    }

    ret = mStaticMetadata->Get(ANDROID_SENSOR_INFO_MAX_FRAME_DURATION, &entry);
    if ((ret == OK) && (entry.count == 1)) {
        mSensorMaxFrameDuration = entry.data.i64[0];
    } else if (!manualSensor) {
        mSensorMaxFrameDuration = EmulatedSensor::kSupportedFrameDurationRange[1];
    } else {
        ALOGE("%s: Manual sensor devices must advertise sensor max frame duration!", __FUNCTION__);
        return BAD_VALUE;
    }

    if (manualSensor) {
        if (mAvailableRequests.find(ANDROID_SENSOR_SENSITIVITY) == mAvailableRequests.end()) {
            ALOGE("%s: Sensor sensitivity must be configurable on manual sensor devices!",
                    __FUNCTION__);
            return BAD_VALUE;
        }

        if (mAvailableRequests.find(ANDROID_SENSOR_EXPOSURE_TIME) == mAvailableRequests.end()) {
            ALOGE("%s: Sensor exposure time must be configurable on manual sensor devices!",
                    __FUNCTION__);
            return BAD_VALUE;
        }

        if (mAvailableRequests.find(ANDROID_SENSOR_FRAME_DURATION) == mAvailableRequests.end()) {
            ALOGE("%s: Sensor frame duration must be configurable on manual sensor devices!",
                    __FUNCTION__);
            return BAD_VALUE;
        }
    }

    if (mAvailableResults.find(ANDROID_SENSOR_TIMESTAMP) == mAvailableRequests.end()) {
        ALOGE("%s: Sensor timestamp must always be part of the results!", __FUNCTION__);
        return BAD_VALUE;
    }

    if (supportsCapability(ANDROID_REQUEST_AVAILABLE_CAPABILITIES_READ_SENSOR_SETTINGS)) {
        if (mAvailableResults.find(ANDROID_SENSOR_SENSITIVITY) == mAvailableRequests.end()) {
            ALOGE("%s: Sensor sensitivity must always be part of the results on devices supporting"
                    " sensor setting reads!", __FUNCTION__);
            return BAD_VALUE;
        }

        if (mAvailableResults.find(ANDROID_SENSOR_EXPOSURE_TIME) == mAvailableRequests.end()) {
            ALOGE("%s: Sensor exposure time must always be part of the results on devices"
                    "supporting sensor setting reads!", __FUNCTION__);
            return BAD_VALUE;
        }

        if (mAvailableResults.find(ANDROID_SENSOR_FRAME_DURATION) == mAvailableRequests.end()) {
            ALOGE("%s: Sensor frame duration must always be part of the results on devices"
                    "supporting sensor setting reads!", __FUNCTION__);
            return BAD_VALUE;
        }
    }

    mSensorExposureTime = getClosestValue(EmulatedSensor::kDefaultExposureTime,
            mSensorExposureTimeRange.first, mSensorExposureTimeRange.second);
    mSensorFrameDuration = getClosestValue(EmulatedSensor::kDefaultFrameDuration,
            EmulatedSensor::kSupportedFrameDurationRange[0], mSensorMaxFrameDuration);
    mSensorSensitivity = getClosestValue(EmulatedSensor::kDefaultSensitivity,
            mSensorSensitivityRange.first, mSensorSensitivityRange.second);

    auto manualIdx = static_cast<size_t>(RequestTemplate::kManual);
    if (mDefaultRequests[manualIdx].get() != nullptr) {
        mDefaultRequests[manualIdx]->Set(ANDROID_SENSOR_EXPOSURE_TIME, &mSensorExposureTime, 1);
        mDefaultRequests[manualIdx]->Set(ANDROID_SENSOR_FRAME_DURATION, &mSensorFrameDuration, 1);
        mDefaultRequests[manualIdx]->Set(ANDROID_SENSOR_SENSITIVITY, &mSensorSensitivity, 1);
    }

    return OK;
}

status_t EmulatedRequestProcessor::initializeControlDefaults() {
    camera_metadata_ro_entry_t entry;
    auto ret = mStaticMetadata->Get(ANDROID_CONTROL_AVAILABLE_MODES, &entry);
    if (ret == OK) {
        mAvailableControlModes.insert(entry.data.u8, entry.data.u8 + entry.count);
    } else {
        ALOGE("%s: No available control modes!", __FUNCTION__);
        return BAD_VALUE;
    }

    // Auto mode must always be present
    auto autoMode = mAvailableControlModes.find(ANDROID_CONTROL_MODE_AUTO);
    if (autoMode == mAvailableControlModes.end()) {
        ALOGE("%s: Auto control modes must always be present!", __FUNCTION__);
        return BAD_VALUE;
    }

    // Capture intent must always be use configurable
    if (mAvailableRequests.find(ANDROID_CONTROL_CAPTURE_INTENT) == mAvailableRequests.end()) {
        ALOGE("%s: Clients must be able to set the capture intent!", __FUNCTION__);
        return BAD_VALUE;
    }

    ret = mStaticMetadata->Get(ANDROID_CONTROL_AE_AVAILABLE_MODES, &entry);
    if (ret == OK) {
        mAvailableAEModes.insert(entry.data.u8, entry.data.u8 + entry.count);
    } else {
        ALOGE("%s: No available AE modes!", __FUNCTION__);
        return BAD_VALUE;
    }
    // On mode must always be present
    autoMode = mAvailableAEModes.find(ANDROID_CONTROL_AE_MODE_ON);
    if (autoMode == mAvailableAEModes.end()) {
        ALOGE("%s: AE on control mode must always be present!", __FUNCTION__);
        return BAD_VALUE;
    }

    // TODO: Add the rest of android.control.* as much as possible

    if (supportsCapability(ANDROID_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR)) {
        auto offMode = mAvailableControlModes.find(ANDROID_CONTROL_MODE_OFF);
        if (offMode == mAvailableControlModes.end()) {
            ALOGE("%s: Off control mode must always be present for manual sensors!", __FUNCTION__);
            return BAD_VALUE;
        }

        offMode = mAvailableAEModes.find(ANDROID_CONTROL_AE_MODE_OFF);
        if (offMode == mAvailableAEModes.end()) {
            ALOGE("%s: AE off control mode must always be present for manual sensors!",
                    __FUNCTION__);
            return BAD_VALUE;
        }
    }

    for (size_t idx = 0; idx < kTemplateCount; idx++) {
        auto templateIdx = static_cast<RequestTemplate> (idx);
        if (mDefaultRequests[idx].get() == nullptr) {
            continue;
        }

        uint8_t intent = ANDROID_CONTROL_CAPTURE_INTENT_CUSTOM;
        uint8_t controlMode, aeMode;
        switch(templateIdx) {
            case RequestTemplate::kManual:
                intent = ANDROID_CONTROL_CAPTURE_INTENT_MANUAL;
                controlMode = ANDROID_CONTROL_MODE_OFF;
                aeMode = ANDROID_CONTROL_AE_MODE_OFF;
                break;
            case RequestTemplate::kPreview:
                intent = ANDROID_CONTROL_CAPTURE_INTENT_PREVIEW;
                controlMode = ANDROID_CONTROL_MODE_AUTO;
                aeMode = ANDROID_CONTROL_AE_MODE_ON;
                break;
            case RequestTemplate::kStillCapture:
                intent = ANDROID_CONTROL_CAPTURE_INTENT_STILL_CAPTURE;
                controlMode = ANDROID_CONTROL_MODE_AUTO;
                aeMode = ANDROID_CONTROL_AE_MODE_ON;
                break;
            case RequestTemplate::kVideoRecord:
                intent = ANDROID_CONTROL_CAPTURE_INTENT_VIDEO_RECORD;
                controlMode = ANDROID_CONTROL_MODE_AUTO;
                aeMode = ANDROID_CONTROL_AE_MODE_ON;
                break;
            case RequestTemplate::kVideoSnapshot:
                intent = ANDROID_CONTROL_CAPTURE_INTENT_VIDEO_SNAPSHOT;
                controlMode = ANDROID_CONTROL_MODE_AUTO;
                aeMode = ANDROID_CONTROL_AE_MODE_ON;
                break;
            default:
                //Noop
                break;
        }

        if (intent != ANDROID_CONTROL_CAPTURE_INTENT_CUSTOM) {
            mDefaultRequests[idx]->Set(ANDROID_CONTROL_CAPTURE_INTENT, &intent, 1);
            mDefaultRequests[idx]->Set(ANDROID_CONTROL_MODE, &controlMode, 1);
            mDefaultRequests[idx]->Set(ANDROID_CONTROL_AE_MODE, &aeMode, 1);
        }
    }

    return initializeSensorDefaults();
}

status_t EmulatedRequestProcessor::initializeInfoDefaults() {
    camera_metadata_ro_entry_t entry;
    auto ret = mStaticMetadata->Get(ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL, &entry);
    if ((ret == OK) && (entry.count == 1)) {
        if (kSupportedHWLevels.find(entry.data.u8[0]) == kSupportedCapabilites.end()) {
            ALOGE("%s: HW Level: %u not supported", __FUNCTION__, entry.data.u8[0]);
            return BAD_VALUE;
        }
    } else {
        ALOGE("%s: No available hardware level!", __FUNCTION__);
        return BAD_VALUE;
    }

    mSupportedHWLevel = entry.data.u8[0];

    return initializeControlDefaults();
}

status_t EmulatedRequestProcessor::initializeDefaultRequests() {
    camera_metadata_ro_entry_t entry;
    auto ret = mStaticMetadata->Get(ANDROID_REQUEST_AVAILABLE_CAPABILITIES, &entry);
    if ((ret == OK) && (entry.count > 0)) {
        for (size_t i = 0; i < entry.count; i++) {
            if (kSupportedCapabilites.find(entry.data.u8[i]) == kSupportedCapabilites.end()) {
                ALOGE("%s: Capability: %u not supported", __FUNCTION__, entry.data.u8[i]);
                return BAD_VALUE;
            }
        }
    } else {
        ALOGE("%s: No available capabilities!", __FUNCTION__);
        return BAD_VALUE;
    }
    mAvailableCapabilites.insert(entry.data.u8, entry.data.u8 + entry.count);

    ret = mStaticMetadata->Get(ANDROID_REQUEST_PIPELINE_MAX_DEPTH, &entry);
    if ((ret == OK) && (entry.count == 1)) {
        if (entry.data.u8[0] == 0) {
            ALOGE("%s: Maximum request pipeline must have a non zero value!", __FUNCTION__);
            return BAD_VALUE;
        }
    } else {
        ALOGE("%s: Maximum request pipeline depth absent!", __FUNCTION__);
        return BAD_VALUE;
    }
    mMaxPipelineDepth = entry.data.u8[0];

    ret = mStaticMetadata->Get(ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS, &entry);
    if ((ret != OK) || (entry.count == 0)) {
        ALOGE("%s: No available characteristic keys!", __FUNCTION__);
        return BAD_VALUE;
    }
    mAvailableCharacteritics.insert(entry.data.i32, entry.data.i32 + entry.count);

    ret = mStaticMetadata->Get(ANDROID_REQUEST_AVAILABLE_RESULT_KEYS, &entry);
    if ((ret != OK) || (entry.count == 0)) {
        ALOGE("%s: No available result keys!", __FUNCTION__);
        return BAD_VALUE;
    }
    mAvailableResults.insert(entry.data.i32, entry.data.i32 + entry.count);

    ret = mStaticMetadata->Get(ANDROID_REQUEST_AVAILABLE_REQUEST_KEYS, &entry);
    if ((ret != OK) || (entry.count == 0)) {
        ALOGE("%s: No available request keys!", __FUNCTION__);
        return BAD_VALUE;
    }
    mAvailableRequests.insert(entry.data.i32, entry.data.i32 + entry.count);

    if (supportsCapability(ANDROID_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR)) {
        auto templateIdx = static_cast<size_t> (RequestTemplate::kManual);
        mDefaultRequests[templateIdx] = HalCameraMetadata::Create(1, 10);
    }

    if (isBackwardCompatible()) {
        for (size_t templateIdx = 0; templateIdx < kTemplateCount; templateIdx++) {
            switch(static_cast<RequestTemplate> (templateIdx)) {
                case RequestTemplate::kPreview:
                case RequestTemplate::kStillCapture:
                case RequestTemplate::kVideoRecord:
                case RequestTemplate::kVideoSnapshot:
                    mDefaultRequests[templateIdx] = HalCameraMetadata::Create(1, 10);
                    break;
                default:
                    //Noop
                    break;
            }
        }
    }

    return initializeInfoDefaults();
}

status_t EmulatedRequestProcessor::initialize(std::unique_ptr<HalCameraMetadata> staticMeta) {
    std::lock_guard<std::mutex> lock(mProcessMutex);
    mStaticMetadata = std::move(staticMeta);

    return initializeDefaultRequests();
}

status_t EmulatedRequestProcessor::retrieveDefaultRequest(RequestTemplate type,
        std::unique_ptr<HalCameraMetadata>* default_settings) {

    if (default_settings == nullptr) {
        ALOGE("%s default_settings is nullptr", __FUNCTION__);
        return BAD_VALUE;
    }

    std::lock_guard<std::mutex> lock(mProcessMutex);
    auto idx = static_cast<size_t>(type);
    if (idx >= kTemplateCount) {
        ALOGE("%s: Unexpected request type: %d", __FUNCTION__, type);
        return BAD_VALUE;
    }

    if (mDefaultRequests[idx].get() == nullptr) {
        ALOGE("%s: Unsupported request type: %d", __FUNCTION__, type);
        return BAD_VALUE;
    }

    *default_settings = HalCameraMetadata::Clone(mDefaultRequests[idx]->GetRawCameraMetadata());

    return OK;
}

}  // namespace android
