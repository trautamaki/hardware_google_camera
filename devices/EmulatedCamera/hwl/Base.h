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

#ifndef HW_EMULATOR_CAMERA_BASE_H
#define HW_EMULATOR_CAMERA_BASE_H

#include "HandleImporter.h"
#include "hwl_types.h"
#include <log/log.h>
#include <memory>

namespace android {

using android::hardware::camera::common::V1_0::helper::HandleImporter;
using google_camera_hal::HwlPipelineCallback;
using google_camera_hal::StreamBuffer;

struct YCbCrPlanes {
    uint8_t *imgY, *imgCb, *imgCr;
    uint32_t yStride, CbCrStride, CbCrStep;
};

struct SinglePlane {
    uint8_t *img;
    uint32_t stride;
    uint32_t bufferSize;
};

struct SensorBuffer {
    uint32_t width, height;
    uint32_t frameNumber;
    uint32_t pipelineId;
    uint32_t cameraId;
    android_pixel_format_t format;
    android_dataspace_t dataSpace;
    StreamBuffer streamBuffer;
    HandleImporter importer;
    HwlPipelineCallback callback;
    int acquireFenceFd;

    union Plane {
        SinglePlane img;
        YCbCrPlanes imgYCrCb;
    } plane;

    SensorBuffer() : width(0), height(0), frameNumber(0), pipelineId(0), cameraId(0),
            format(HAL_PIXEL_FORMAT_RGBA_8888), dataSpace(HAL_DATASPACE_UNKNOWN),
            streamBuffer{0}, acquireFenceFd(-1) {}

    SensorBuffer(const SensorBuffer&) = delete;
    SensorBuffer& operator = (const SensorBuffer&) = delete;
};

typedef std::vector<std::unique_ptr<SensorBuffer>> Buffers;

}  // namespace android

using android::google_camera_hal::BufferStatus;
using android::google_camera_hal::ErrorCode;
using android::google_camera_hal::HwlPipelineResult;
using android::google_camera_hal::NotifyMessage;
using android::google_camera_hal::MessageType;

template<>
struct std::default_delete<android::SensorBuffer> {
    inline void operator() (android::SensorBuffer *buffer) const {
        if (buffer != nullptr) {
            if (buffer->streamBuffer.buffer != nullptr) {
                buffer->importer.unlock(buffer->streamBuffer.buffer);
            }

            if (buffer->acquireFenceFd >= 0) {
                buffer->importer.closeFence(buffer->acquireFenceFd);
            }

            if ((buffer->streamBuffer.status != BufferStatus::kOk) &&
                    (buffer->callback.notify != nullptr)) {
                NotifyMessage msg = {
                    .type = MessageType::kError,
                    .message.error = {
                        .frame_number = buffer->frameNumber,
                        .error_stream_id = buffer->streamBuffer.stream_id,
                        .error_code = ErrorCode::kErrorBuffer
                    }
                };
                buffer->callback.notify(buffer->pipelineId, msg);
            }

            if (buffer->callback.process_pipeline_result != nullptr) {
                auto result = std::make_unique<HwlPipelineResult>();
                result->camera_id = buffer->cameraId;
                result->pipeline_id = buffer->pipelineId;
                result->frame_number = buffer->frameNumber;
                result->partial_result = 0;

                buffer->streamBuffer.acquire_fence = buffer->streamBuffer.release_fence = nullptr;
                result->output_buffers.push_back(buffer->streamBuffer);
                buffer->callback.process_pipeline_result(std::move(result));
            }
            delete buffer;
        }
    }
};

#endif
