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

#define LOG_TAG "StreamConfigurationMap"
#include <log/log.h>

#include "StreamConfigurationMap.h"

namespace android {

StreamConfigurationMap::StreamConfigurationMap(const HalCameraMetadata& chars) {
    const size_t STREAM_FORMAT_OFFSET = 0;
    const size_t STREAM_WIDTH_OFFSET = 1;
    const size_t STREAM_HEIGHT_OFFSET = 2;
    const size_t STREAM_IS_INPUT_OFFSET = 3;
    const size_t STREAM_MIN_DURATION_OFFSET = 3;
    const size_t STREAM_STALL_DURATION_OFFSET = 3;
    const size_t STREAM_CONFIGURATION_SIZE = 4;

    camera_metadata_ro_entry_t entry;
    auto ret = chars.Get(ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &entry);
    if (ret != OK) {
        ALOGW("%s: ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS missing!", __FUNCTION__);
        entry.count = 0;
    }

    for (size_t i = 0; i < entry.count; i+= STREAM_CONFIGURATION_SIZE) {
        int32_t width = entry.data.i32[i + STREAM_WIDTH_OFFSET];
        int32_t height = entry.data.i32[i + STREAM_HEIGHT_OFFSET];
        auto format = static_cast<android_pixel_format_t> (
                entry.data.i32[i + STREAM_FORMAT_OFFSET]);
        int32_t isInput = entry.data.i32[i + STREAM_IS_INPUT_OFFSET];
        if (!isInput) {
            mStreamOutputFormats.insert(format);
            mStreamOutputSizeMap[format].insert(std::make_pair(width, height));
        }
    }

    ret = chars.Get(ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS, &entry);
    if (ret != OK) {
        ALOGW("%s: ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS missing!", __FUNCTION__);
        entry.count = 0;
    }

    for (size_t i = 0; i < entry.count; i+= STREAM_CONFIGURATION_SIZE) {
        auto format = static_cast<android_pixel_format_t> (
                entry.data.i64[i + STREAM_FORMAT_OFFSET]);
        uint32_t width = entry.data.i64[i + STREAM_WIDTH_OFFSET];
        uint32_t height = entry.data.i64[i + STREAM_HEIGHT_OFFSET];
        nsecs_t duration = entry.data.i64[i + STREAM_MIN_DURATION_OFFSET];
        auto streamConfiguration = std::make_pair(format, std::make_pair(width, height));
        mStreamMinDurationMap[streamConfiguration] = duration;
    }

    ret = chars.Get(ANDROID_SCALER_AVAILABLE_STALL_DURATIONS, &entry);
    if (ret != OK) {
        ALOGW("%s: ANDROID_SCALER_AVAILABLE_STALL_DURATIONS missing!", __FUNCTION__);
        entry.count = 0;
    }

    for (size_t i = 0; i < entry.count; i+= STREAM_CONFIGURATION_SIZE) {
        auto format = static_cast<android_pixel_format_t> (
                entry.data.i64[i + STREAM_FORMAT_OFFSET]);
        uint32_t width = entry.data.i64[i + STREAM_WIDTH_OFFSET];
        uint32_t height = entry.data.i64[i + STREAM_HEIGHT_OFFSET];
        nsecs_t duration = entry.data.i64[i + STREAM_STALL_DURATION_OFFSET];
        auto streamConfiguration = std::make_pair(format, std::make_pair(width, height));
        mStreamStallMap[streamConfiguration] = duration;
    }
}

}  // namespace android
