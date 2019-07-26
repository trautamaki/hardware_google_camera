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

#ifndef HW_EMULATOR_CAMERA_JPEG_H
#define HW_EMULATOR_CAMERA_JPEG_H

#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>

#include "Base.h"
#include "HandleImporter.h"

#include <hwl_types.h>

extern "C" {
#include <jpeglib.h>
}

namespace android {

using android::hardware::camera::common::V1_0::helper::HandleImporter;
using google_camera_hal::BufferStatus;
using google_camera_hal::HwlPipelineCallback;
using google_camera_hal::HwlPipelineResult;

struct JpegJob {
    std::unique_ptr<SensorBuffer> input, output;
    std::unique_ptr<HwlPipelineResult> result;
    std::unique_ptr<HwlPipelineCallback> callback;
};

class JpegCompressor {
public:
    JpegCompressor();
    virtual ~JpegCompressor();

    status_t queue(std::unique_ptr<JpegJob> job);

private:
    std::mutex mMutex;
    std::condition_variable mCondition;
    bool mJpegDone = false;
    std::thread mJpegProcessingThread;
    HandleImporter mImporter;
    std::queue<std::unique_ptr<JpegJob>> mPendingJobs;

    j_common_ptr mJpegErrorInfo;
    jpeg_compress_struct mCInfo;
    bool checkError(const char *msg);
    void compress(std::unique_ptr<JpegJob> job);
    void cleanUp(std::unique_ptr<JpegJob> job, BufferStatus status);
    void threadLoop();

    JpegCompressor(const JpegCompressor&) = delete;
    JpegCompressor& operator = (const JpegCompressor&) = delete;
};

}  // namespace android

#endif
