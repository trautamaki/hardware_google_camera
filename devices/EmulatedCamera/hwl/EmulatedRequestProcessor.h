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

#ifndef EMULATOR_CAMERA_HAL_HWL_REQUEST_PROCESSOR_H
#define EMULATOR_CAMERA_HAL_HWL_REQUEST_PROCESSOR_H

#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>

#include "EmulatedSensor.h"
#include "hwl_types.h"

namespace android {

using google_camera_hal::HalCameraMetadata;
using google_camera_hal::HalStream;
using google_camera_hal::HwlPipelineCallback;
using google_camera_hal::HwlPipelineRequest;
using google_camera_hal::StreamBuffer;

struct EmulatedStream : public HalStream {
    uint32_t width, height;
    size_t bufferSize;
};

struct EmulatedPipeline {
    HwlPipelineCallback cb;
    // stream id -> stream map
    std::unordered_map<uint32_t, EmulatedStream> streams;
    uint32_t physicalCameraId, pipelineId;
};

class EmulatedRequestProcessor {
public:
    EmulatedRequestProcessor(uint32_t cameraId, uint8_t maxPipelineDepth,
            sp<EmulatedSensor> sensor);
    virtual ~EmulatedRequestProcessor();

    // Process given pipeline requests and invoke the respective callback in a separate thread
    status_t processPipelineRequests(uint32_t frameNumber,
            const std::vector<HwlPipelineRequest>& requests,
            const std::vector<EmulatedPipeline>& pipelines);

    status_t flush();

private:

    void requestProcessorLoop();

    std::mutex mProcessMutex;
    std::condition_variable mRequestCondition;
    std::thread mRequestThread;
    bool mProcessorDone = false;

    struct PendingRequest {
        std::unique_ptr<HalCameraMetadata> settings;
        std::vector<StreamBuffer> inputBuffers;
        // TODO: input buffer meta
        std::unique_ptr<Buffers> outputBuffers;
    };

    static uint32_t inline alignTo(uint32_t value, uint32_t alignment) {
        uint32_t delta = value % alignment;
        return (delta == 0) ? value : (value + (alignment - delta));
    }

    status_t getBufferSizeAndStride(const EmulatedStream& stream, uint32_t *size /*out*/,
            uint32_t *stride /*out*/);
    status_t lockSensorBuffer(const EmulatedStream& stream, HandleImporter& importer /*in*/,
            buffer_handle_t buffer, SensorBuffer *sensorBuffer /*out*/);
    std::unique_ptr<SensorBuffer> createSensorBuffer(uint32_t frameNumber,
            const EmulatedStream& stream, const HwlPipelineRequest& request,
            HwlPipelineCallback callback, StreamBuffer streamBuffer);
    std::unique_ptr<Buffers> initializeOutputBuffers(const PendingRequest& request);
    std::unique_ptr<HwlPipelineResult> initializeResult(const PendingRequest& request,
            uint32_t pipelineId, uint32_t frameNumber);

    std::queue<PendingRequest> mPendingRequests;
    uint8_t mMaxPipelineDepth;
    uint32_t mCameraId;
    sp<EmulatedSensor> mSensor;

    EmulatedRequestProcessor(const EmulatedRequestProcessor&) = delete;
    EmulatedRequestProcessor& operator = (const EmulatedRequestProcessor&) = delete;
};

}  // namespace android

#endif  // EMULATOR_CAMERA_HAL_HWL_REQUEST_PROCESSOR_H
