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

struct JpegYUV420Input {
    uint32_t width, height;
    bool bufferOwner;
    YCbCrPlanes yuvPlanes;

    JpegYUV420Input() : width(0), height(0), bufferOwner(false) {}
    ~JpegYUV420Input() {
        if ((yuvPlanes.imgY != nullptr) && bufferOwner) {
            delete [] yuvPlanes.imgY;
            yuvPlanes = {};
        }
    }

    JpegYUV420Input(const JpegYUV420Input&) = delete;
    JpegYUV420Input& operator = (const JpegYUV420Input&) = delete;
};

struct JpegYUV420Job {
    std::unique_ptr<JpegYUV420Input> input;
    std::unique_ptr<SensorBuffer> output;
    std::unique_ptr<HalCameraMetadata> resultMetadata;
};

class JpegCompressor {
public:
    JpegCompressor(std::unique_ptr<ExifUtils> exifUtils);
    virtual ~JpegCompressor();

    status_t queueYUV420(std::unique_ptr<JpegYUV420Job> job);

private:
    std::mutex mMutex;
    std::condition_variable mCondition;
    bool mJpegDone = false;
    std::thread mJpegProcessingThread;
    std::queue<std::unique_ptr<JpegYUV420Job>> mPendingYUVJobs;
    std::unique_ptr<ExifUtils> mExifUtils;

    j_common_ptr mJpegErrorInfo;
    bool checkError(const char *msg);
    void compressYUV420(std::unique_ptr<JpegYUV420Job> job);
    struct YUV420Frame {
        uint8_t *outputBuffer;
        size_t outputBufferSize;
        YCbCrPlanes yuvPlanes;
        size_t width;
        size_t height;
        const uint8_t *app1Buffer;
        size_t app1BufferSize;
    };
    size_t compressYUV420Frame(YUV420Frame frame);
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
