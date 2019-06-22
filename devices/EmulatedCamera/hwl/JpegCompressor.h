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

struct JpegRGBInput {
    uint32_t width, height;
    uint32_t stride;
    uint8_t *img;

    JpegRGBInput() : width(0), height(0), stride(0), img(nullptr) {}
    ~JpegRGBInput() {
        if (img != nullptr) {
            delete [] img;
            img = nullptr;
        }
    }

    JpegRGBInput(const JpegRGBInput&) = delete;
    JpegRGBInput& operator = (const JpegRGBInput&) = delete;
};

struct JpegJob {
    std::unique_ptr<JpegRGBInput> input;
    std::unique_ptr<SensorBuffer> output;
    std::unique_ptr<HwlPipelineResult> result;

    ~JpegJob() {
        if (output->callback.process_pipeline_result != nullptr) {
            if (result->result_metadata.get() != nullptr) {
                result->partial_result = 1;
            }
            output->callback.process_pipeline_result(std::move(result));
        }
    }
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
    bool checkError(const char *msg);
    void compress(std::unique_ptr<JpegJob> job);
    void threadLoop();

    JpegCompressor(const JpegCompressor&) = delete;
    JpegCompressor& operator = (const JpegCompressor&) = delete;
};

}  // namespace android

template<>
struct std::default_delete<jpeg_compress_struct> {
    inline void operator() (jpeg_compress_struct *cinfo) const {
        jpeg_destroy_compress(cinfo);
    }
};

#endif
