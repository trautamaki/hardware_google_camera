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

#include "utils/ExifUtils.h"

namespace android {

using android::hardware::camera::common::V1_0::helper::HandleImporter;
using google_camera_hal::BufferStatus;
using google_camera_hal::HwlPipelineCallback;
using google_camera_hal::HwlPipelineResult;

struct JpegARGBInput {
    uint32_t width, height;
    uint32_t stride;
    uint8_t *img;

    JpegARGBInput() : width(0), height(0), stride(0), img(nullptr) {}
    ~JpegARGBInput() {
        if (img != nullptr) {
            delete [] img;
            img = nullptr;
        }
    }

    JpegARGBInput(const JpegARGBInput&) = delete;
    JpegARGBInput& operator = (const JpegARGBInput&) = delete;
};

struct JpegJob {
    std::unique_ptr<JpegARGBInput> input;
    std::unique_ptr<SensorBuffer> output;
    std::unique_ptr<HalCameraMetadata> resultMetadata;
};

class JpegCompressor {
public:
    JpegCompressor(std::unique_ptr<ExifUtils> exifUtils);
    virtual ~JpegCompressor();

    status_t queue(std::unique_ptr<JpegJob> job);

private:
    std::mutex mMutex;
    std::condition_variable mCondition;
    bool mJpegDone = false;
    std::thread mJpegProcessingThread;
    HandleImporter mImporter;
    std::queue<std::unique_ptr<JpegJob>> mPendingJobs;
    std::unique_ptr<ExifUtils> mExifUtils;

    j_common_ptr mJpegErrorInfo;
    bool checkError(const char *msg);
    void compress(std::unique_ptr<JpegJob> job);
    struct ARGBFrame {
        uint8_t *outputBuffer;
        size_t outputBufferSize;
        uint8_t *inputBuffer;
        size_t inputBufferStride;
        size_t width;
        size_t height;
        const uint8_t *app1Buffer;
        size_t app1BufferSize;
    };
    size_t compressARGBFrame(ARGBFrame frame);
    void threadLoop();

    JpegCompressor(const JpegCompressor&) = delete;
    JpegCompressor& operator = (const JpegCompressor&) = delete;
};

}  // namespace android

template<>
struct std::default_delete<jpeg_compress_struct> {
    inline void operator() (jpeg_compress_struct *cinfo) const {
        if (cinfo != nullptr) {
            jpeg_destroy_compress(cinfo);
            delete cinfo;
        }
    }
};

#endif
