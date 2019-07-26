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

#define LOG_TAG "EmulatedRequestProcessor"
#define ATRACE_TAG ATRACE_TAG_CAMERA

#include "HandleImporter.h"
#include <log/log.h>
#include <utils/Timers.h>
#include <utils/Trace.h>

#include "EmulatedRequestProcessor.h"

namespace android {

using android::hardware::camera::common::V1_0::helper::HandleImporter;
using google_camera_hal::ErrorCode;
using google_camera_hal::HwlPipelineResult;
using google_camera_hal::NotifyMessage;
using google_camera_hal::MessageType;

EmulatedRequestProcessor::EmulatedRequestProcessor(uint8_t maxPipelineDepth,
        sp<EmulatedSensor> sensor) :
    mMaxPipelineDepth(maxPipelineDepth),
    mSensor(sensor) {
    ATRACE_CALL();
    mRequestThread = std::thread([this] { this->requestProcessorLoop(); });
}

EmulatedRequestProcessor::~EmulatedRequestProcessor() {
    ATRACE_CALL();
    {
        std::lock_guard<std::mutex> lock(mProcessMutex);
        mProcessorDone = true;
        mRequestCondition.notify_one();
    }
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
            auto result = mRequestCondition.wait_for(lock, std::chrono::milliseconds(1000));
            if (result == std::cv_status::timeout) {
                ALOGE("%s Timed out waiting for a pending request slot", __FUNCTION__);
                return TIMED_OUT;
            }
        }

        std::unordered_map<int32_t, EmulatedStream> streamMap;
        for (const auto &halStream : pipelines[request.pipeline_id].streams) {
            streamMap.emplace(halStream.id, halStream);
        }

        mPendingRequests.push({
                .frameNumber = frameNumber,
                .callback = pipelines[request.pipeline_id].cb,
                .pipelineId = request.pipeline_id,
                .settings = HalCameraMetadata::Clone(request.settings.get()),
                .inputBuffers = request.input_buffers,
                .outputBuffers = request.output_buffers,
                .streamMap = std::move(streamMap) });

        //TODO duplicate fences
    }

    return OK;
}

status_t EmulatedRequestProcessor::flush() {
    std::lock_guard<std::mutex> lock(mProcessMutex);
    // First flush in-flight requests
    auto ret = mSensor->flush();

    // Then the rest of the pending requests
    while (!mPendingRequests.empty()) {
        const auto& request = mPendingRequests.front();

        if (request.callback.notify != nullptr) {
            NotifyMessage msg = {
                .type = MessageType::kError,
                .message.error = {
                    .frame_number = request.frameNumber,
                    .error_stream_id = -1,
                    .error_code = ErrorCode::kErrorRequest
                }
            };
            request.callback.notify(request.pipelineId, msg);
        }

        mPendingRequests.pop();
    }

    return ret;
}

inline uint32_t alignTo(uint32_t value, uint32_t alignment) {
    uint32_t delta = value % alignment;
    return (delta == 0) ? value : (value + (alignment - delta));
}

status_t getBufferSizeAndStride(const EmulatedStream& stream, uint32_t *size /*out*/,
        uint32_t *stride /*out*/) {
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

status_t lockSensorBuffer(const EmulatedStream& stream, HandleImporter& importer /*in*/,
        buffer_handle_t buffer, SensorBuffer *sensorBuffer /*out*/) {
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
                return BAD_VALUE;
            }
            ALOGE("%s: Mapped address: %p", __func__, sensorBuffer->plane.img.img);
        } else {
            ALOGE("%s: Unsupported pixel format: 0x%x", __FUNCTION__,
                    stream.override_format);
            return BAD_VALUE;
        }
    }

    return OK;
}

void EmulatedRequestProcessor::requestProcessorLoop() {
    ATRACE_CALL();

    nsecs_t maxFrameDurationNs = s2ns(1); // TODO: set accordingly
    HandleImporter importer;
    while (!mProcessorDone) {
        {
            std::lock_guard<std::mutex> lock(mProcessMutex);
            if (!mPendingRequests.empty()) {
                const auto& request = mPendingRequests.front();
                std::vector<StreamBuffer> outputBuffers;
                std::unique_ptr<Buffers> sensorBuffers = std::make_unique<Buffers>();

                sensorBuffers->reserve(outputBuffers.size());
                outputBuffers.reserve(outputBuffers.size());

                outputBuffers.insert(outputBuffers.end(), request.outputBuffers.begin(),
                        request.outputBuffers.end());
                //TODO take care of the fences
                for (auto& outputBuffer : outputBuffers) {
                    const auto& stream = request.streamMap.at(outputBuffer.stream_id);
                    SensorBuffer sensorBuffer = {
                            .width = stream.width,
                            .height = stream.height,
                            .format = stream.override_format,
                            .dataSpace = stream.override_data_space,
                            .streamBuffer = outputBuffer,
                    };

                    auto ret = lockSensorBuffer(stream, importer, outputBuffer.buffer,
                            &sensorBuffer);
                    if (ret == OK) {
                        sensorBuffers->push_back(sensorBuffer);
                    } else {
                        //TODO: return failed buffers immediately
                        outputBuffer.status = google_camera_hal::BufferStatus::kError;
                    }
                }

                auto result = std::make_unique<HwlPipelineResult>();
                result->camera_id = 0; //TODO
                result->pipeline_id = request.pipelineId;
                result->frame_number = request.frameNumber;
                result->result_metadata = request.settings != nullptr ?
                    HalCameraMetadata::Clone(request.settings.get()) :
                    HalCameraMetadata::Create(1, 10);
                result->result_metadata->Set(ANDROID_REQUEST_PIPELINE_DEPTH, &mMaxPipelineDepth, 1);
                result->input_buffers = request.inputBuffers;
                result->partial_result = 1; //TODO: Change in case of partial result support

                EmulatedSensor::SensorSettings settings(request.callback, us2ns(33), ms2ns(33),
                        100 /*ISO*/, request.frameNumber);

                mSensor->setCurrentRequest(settings, std::move(result), std::move(sensorBuffers));
                mPendingRequests.pop();
                mRequestCondition.notify_one();
            }
        }

        mSensor->waitForVSync(maxFrameDurationNs);
    }
}

}  // namespace android
