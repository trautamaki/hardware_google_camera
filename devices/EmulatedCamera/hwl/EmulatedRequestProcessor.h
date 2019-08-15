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

#include "hwl_types.h"

namespace android {

using google_camera_hal::HalCameraMetadata;
using google_camera_hal::HalStream;
using google_camera_hal::HwlPipelineCallback;
using google_camera_hal::HwlPipelineRequest;
using google_camera_hal::StreamBuffer;

struct EmulatedPipeline {
    HwlPipelineCallback cb;
    std::vector<HalStream> streams;
    uint32_t physicalCameraId, pipelineId;
};

class EmulatedRequestProcessor {
public:
    EmulatedRequestProcessor();
    virtual ~EmulatedRequestProcessor();

    // Process given pipeline requests and invoke the respective callback in a separate thread
    status_t processPipelineRequests(uint32_t frameNumber,
            const std::vector<HwlPipelineRequest>& requests,
            const std::vector<EmulatedPipeline>& pipelines);

private:

    void RequestProcessorLoop();

    std::mutex mProcessMutex;
    std::thread mRequestThread;
    bool mProcessorDone = false;

    struct PendingRequest {
        uint32_t frameNumber;
        uint32_t pipelineId;
        const HwlPipelineCallback& callback;
        std::unique_ptr<HalCameraMetadata> settings;
        std::vector<StreamBuffer> inputBuffers;
        // TODO: input buffer meta
        std::vector<StreamBuffer> outputBuffers;
    };

    std::condition_variable mRequestCondition;
    std::mutex mRequestMutex;
    std::queue<PendingRequest> mPendingRequests;
};

}  // namespace android

#endif  // EMULATOR_CAMERA_HAL_HWL_REQUEST_PROCESSOR_H
