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

#include <log/log.h>
#include <utils/Timers.h>
#include <utils/Trace.h>

#include "EmulatedRequestProcessor.h"

namespace android {

using google_camera_hal::HwlPipelineResult;
using google_camera_hal::NotifyMessage;
using google_camera_hal::MessageType;

EmulatedRequestProcessor::EmulatedRequestProcessor() {
    ATRACE_CALL();
    mRequestThread = std::thread([this] { this->RequestProcessorLoop(); });
}

EmulatedRequestProcessor::~EmulatedRequestProcessor() {
    ATRACE_CALL();
    {
        std::lock_guard<std::mutex> lock(mProcessMutex);
        mProcessorDone = true;
    }
    mRequestCondition.notify_one();
    mRequestThread.join();
}

status_t EmulatedRequestProcessor::processPipelineRequests(uint32_t frameNumber,
        const std::vector<HwlPipelineRequest>& requests,
        const std::vector<EmulatedPipeline>& pipelines) {
    ATRACE_CALL();

    std::lock_guard<std::mutex> lock(mRequestMutex);
    for (const auto& request : requests) {
        if (request.pipeline_id >= pipelines.size()) {
            ALOGE("%s: Pipeline request with invalid pipeline id: %u", __FUNCTION__,
                    request.pipeline_id);
            return BAD_VALUE;
        }

        mPendingRequests.push(PendingRequest {
                .frameNumber = frameNumber,
                .callback = pipelines[request.pipeline_id].cb,
                .pipelineId = request.pipeline_id,
                .settings = HalCameraMetadata::Clone(request.settings.get()),
                .inputBuffers = request.input_buffers,
                .outputBuffers = request.output_buffers });
        //TODO duplicate fences
    }

    mRequestCondition.notify_one();

    return OK;
}

void EmulatedRequestProcessor::RequestProcessorLoop() {
    ATRACE_CALL();

    while (!mProcessorDone) {
        {
            std::lock_guard<std::mutex> lock(mRequestMutex);
            while (!mPendingRequests.empty()) {
                const auto& request = mPendingRequests.front();

                //TODO take care of the fences
                // Shutter message
                NotifyMessage msg {
                    .type = MessageType::kShutter,
                        .message.shutter = {
                            .frame_number = request.frameNumber,
                            .timestamp_ns = static_cast<uint64_t>(systemTime(CLOCK_MONOTONIC))}};
                request.callback.notify(request.pipelineId, msg);

                // Result message
                std::unique_ptr<HwlPipelineResult> result = std::make_unique<HwlPipelineResult> ();
                result->camera_id = 0; //TODO
                result->pipeline_id = request.pipelineId;
                result->frame_number = request.frameNumber;
                result->result_metadata = request.settings != nullptr ?
                        HalCameraMetadata::Clone(request.settings.get()) :
                        HalCameraMetadata::Create(1, 10);
                int64_t sensorTs = msg.message.shutter.timestamp_ns;
                result->result_metadata->Set(ANDROID_SENSOR_TIMESTAMP, &sensorTs, 1);
                result->input_buffers = request.inputBuffers;
                result->output_buffers = request.outputBuffers;
                result->partial_result = 1; //TODO: Change in case of partial result support

                request.callback.process_pipeline_result(std::move(result));

                mPendingRequests.pop();
                usleep(10000); // For testing purposes only!
            }
        }

        if (mProcessorDone) {
            break;
        }

        std::unique_lock<std::mutex> lock(mProcessMutex);

        auto result = mRequestCondition.wait_for(lock, std::chrono::milliseconds(1000));
        if (result == std::cv_status::timeout) {
            ALOGV("%s Timed out waiting for a request to process", __FUNCTION__);
            // Continue waiting until exit is requested
        }
    }
}

}  // namespace android
