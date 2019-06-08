/*
 * Copyright (C) 2012 The Android Open Source Project
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

/**
 * This file includes various basic structures that are needed by multiple parts
 * of the fake camera 2 implementation.
 */

#ifndef HW_EMULATOR_CAMERA2_BASE_H
#define HW_EMULATOR_CAMERA2_BASE_H

#include <utils/Vector.h>

namespace android {

struct YCbCrPlanes {
    uint8_t *imgY, *imgCb, *imgCr;
    uint32_t yStride, CbCrStride, CbCrStep;
};

struct SinglePlane {
    uint8_t *img;
    uint32_t stride;
};

/* Internal structure for passing buffers across threads */
struct SensorBuffer {
    // Positive numbers are output streams
    // Negative numbers are input reprocess streams
    // Zero is an auxillary buffer
    int streamId;
    uint32_t width, height;
    uint32_t format;
    uint32_t dataSpace;
    buffer_handle_t *buffer;

    union Plane {
        SinglePlane img;
        YCbCrPlanes imgYCrCb;
    } plane;
};

typedef Vector<SensorBuffer> Buffers;

}  // namespace android

#endif
