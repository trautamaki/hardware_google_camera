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

EmulatedRequestProcessor::EmulatedRequestProcessor(uint32_t cameraId, sp<EmulatedSensor> sensor) :
    mCameraId(cameraId),
    mSensor(sensor),
    mRequestState(std::make_unique<EmulatedRequestState>(cameraId)) {
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

        while (mPendingRequests.size() > EmulatedSensor::kPipelineDepth) {
            auto result = mRequestCondition.wait_for(lock,
                    std::chrono::nanoseconds(EmulatedSensor::kSupportedFrameDurationRange[1]));
            if (result == std::cv_status::timeout) {
                ALOGE("%s Timed out waiting for a pending request slot", __FUNCTION__);
                return TIMED_OUT;
            }
        }

        auto outputBuffers = createSensorBuffers(frameNumber, request.output_buffers,
                pipelines[request.pipeline_id].streams, request.pipeline_id,
                pipelines[request.pipeline_id].cb);
        auto inputBuffers = createSensorBuffers(frameNumber, request.input_buffers,
                pipelines[request.pipeline_id].streams, request.pipeline_id,
                pipelines[request.pipeline_id].cb);

        mPendingRequests.push({
                .settings = HalCameraMetadata::Clone(request.settings.get()),
                .inputBuffers = std::move(inputBuffers),
                .outputBuffers = std::move(outputBuffers)});
    }

    return OK;
}

std::unique_ptr<Buffers> EmulatedRequestProcessor::createSensorBuffers(uint32_t frameNumber,
        const std::vector<StreamBuffer>& buffers,
        const std::unordered_map<uint32_t, EmulatedStream>& streams, uint32_t pipelineId,
        HwlPipelineCallback cb) {
    if (buffers.empty()) {
        return nullptr;
    }

    auto sensorBuffers = std::make_unique<Buffers>();
    sensorBuffers->reserve(buffers.size());
    for (const auto& buffer : buffers) {
        auto sensorBuffer = createSensorBuffer(frameNumber, streams.at(buffer.stream_id),
                pipelineId, cb, buffer);
        if (sensorBuffer.get() != nullptr) {
            sensorBuffers->push_back(std::move(sensorBuffer));
        }
    }

    return sensorBuffers;
}

void EmulatedRequestProcessor::notifyFailedRequest(const PendingRequest& request) {
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
        const EmulatedStream& emulatedStream, uint32_t pipelineId, HwlPipelineCallback callback,
        StreamBuffer streamBuffer) {
    auto buffer = std::make_unique<SensorBuffer>();

    auto stream = emulatedStream;
    // Make sure input stream formats are correctly mapped here
    if (stream.isInput) {
        stream.override_format = EmulatedSensor::overrideFormat(stream.override_format);
    }
    buffer->width = stream.width;
    buffer->height = stream.height;
    buffer->format = stream.override_format;
    buffer->dataSpace = stream.override_data_space;
    buffer->streamBuffer = streamBuffer;
    buffer->pipelineId = pipelineId;
    buffer->callback = callback;
    buffer->frameNumber = frameNumber;
    buffer->cameraId = mCameraId;
    buffer->isInput = stream.isInput;
    // In case buffer processing is successful, flip this flag accordingly
    buffer->streamBuffer.status = BufferStatus::kError;

    auto ret = lockSensorBuffer(stream, buffer->importer, streamBuffer.buffer, buffer.get());
    if (ret != OK) {
        buffer.release();
        buffer = nullptr;
    }

    if (streamBuffer.acquire_fence != nullptr) {
        auto fenceStatus = buffer->importer.importFence(streamBuffer.acquire_fence,
                buffer->acquireFenceFd);
        if (!fenceStatus) {
            ALOGE("%s: Failed importing acquire fence!", __FUNCTION__);
            buffer.release();
            buffer = nullptr;
        }
    }

    return buffer;
}

std::unique_ptr<Buffers> EmulatedRequestProcessor::acquireBuffers(
        Buffers *buffers) {
    if ((buffers == nullptr) || (buffers->empty())) {
        return nullptr;
    }

    auto acquiredBuffers = std::make_unique<Buffers>();
    acquiredBuffers->reserve(buffers->size());
    auto outputBuffer = buffers->begin();
    while (outputBuffer != buffers->end()) {
        status_t ret = OK;
        if((*outputBuffer)->acquireFenceFd >= 0) {
            ret = sync_wait((*outputBuffer)->acquireFenceFd,
                    ns2ms(EmulatedSensor::kSupportedFrameDurationRange[1]));
            if (ret != OK) {
                ALOGE("%s: Fence sync failed: %s, (%d)", __FUNCTION__, strerror(-ret),
                        ret);
            }
        }

        if (ret == OK) {
            acquiredBuffers->push_back(std::move(*outputBuffer));
        }

        outputBuffer = buffers->erase(outputBuffer);
    }

    return acquiredBuffers;
}

void EmulatedRequestProcessor::requestProcessorLoop() {
    ATRACE_CALL();

    while (!mProcessorDone) {
        {
            std::lock_guard<std::mutex> lock(mProcessMutex);
            if (!mPendingRequests.empty()) {
                status_t ret;
                const auto& request = mPendingRequests.front();
                auto frameNumber = request.outputBuffers->at(0)->frameNumber;
                auto notifyCallback = request.outputBuffers->at(0)->callback;
                auto pipelineId = request.outputBuffers->at(0)->pipelineId;
                EmulatedSensor::SensorSettings settings;

                // Repeating requests usually include valid settings only during the initial
                // call. Afterwards an invalid settings pointer means that there are no changes
                // in the parameters and Hal should re-use the last valid values.
                if (request.settings.get() != nullptr) {
                    ret = mRequestState->initializeSensorSettings(HalCameraMetadata::Clone(
                                request.settings.get()), &settings);
                    mLastSettings = HalCameraMetadata::Clone(request.settings.get());
                } else {
                    ret = mRequestState->initializeSensorSettings(HalCameraMetadata::Clone(
                                mLastSettings.get()), &settings);
                }

                auto outputBuffers = acquireBuffers(request.outputBuffers.get());
                if (!outputBuffers->empty() && (ret == OK)) {
                    auto result = mRequestState->initializeResult(pipelineId, frameNumber);
                    auto inputBuffers = acquireBuffers(request.inputBuffers.get());
                    mSensor->setCurrentRequest(settings, std::move(result),
                            std::move(inputBuffers), std::move(outputBuffers));
                } else {
                    // No further processing is needed, just fail the result which will complete
                    // this request.
                    NotifyMessage msg {
                        .type = MessageType::kError,
                            .message.error = {
                                .frame_number = frameNumber,
                                .error_stream_id = -1,
                                .error_code = ErrorCode::kErrorResult,
                            }
                    };

                    notifyCallback.notify(pipelineId, msg);
                }

                mPendingRequests.pop();
                mRequestCondition.notify_one();
            }
        }

        mSensor->waitForVSync(EmulatedSensor::kSupportedFrameDurationRange[1]);
    }
}

status_t EmulatedRequestProcessor::initialize(std::unique_ptr<HalCameraMetadata> staticMeta) {
    std::lock_guard<std::mutex> lock(mProcessMutex);
    return mRequestState->initialize(std::move(staticMeta));
}

status_t EmulatedRequestProcessor::getDefaultRequest(RequestTemplate type,
        std::unique_ptr<HalCameraMetadata>* default_settings) {
    std::lock_guard<std::mutex> lock(mProcessMutex);
    return mRequestState->getDefaultRequest(type, default_settings);
}

}  // namespace android
