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
void StreamConfigurationMap::appendAvailableStreamConfigurations(
        const camera_metadata_ro_entry& entry) {
    for (size_t i = 0; i < entry.count; i+= kStreamConfigurationSize) {
        int32_t width = entry.data.i32[i + kStreamWidthOffset];
        int32_t height = entry.data.i32[i + kStreamHeightOffset];
        auto format = static_cast<android_pixel_format_t> (
                entry.data.i32[i + kStreamFormatOffset]);
        int32_t isInput = entry.data.i32[i + kStreamIsInputOffset];
        if (!isInput) {
            mStreamOutputFormats.insert(format);
            mStreamOutputSizeMap[format].insert(std::make_pair(width, height));
        }
    }
}

void StreamConfigurationMap::appendAvailableStreamMinDurations(
        const camera_metadata_ro_entry_t& entry) {
    for (size_t i = 0; i < entry.count; i+= kStreamConfigurationSize) {
        auto format = static_cast<android_pixel_format_t> (
                entry.data.i64[i + kStreamFormatOffset]);
        uint32_t width = entry.data.i64[i + kStreamWidthOffset];
        uint32_t height = entry.data.i64[i + kStreamHeightOffset];
        nsecs_t duration = entry.data.i64[i + kStreamMinDurationOffset];
        auto streamConfiguration = std::make_pair(format, std::make_pair(width, height));
        mStreamMinDurationMap[streamConfiguration] = duration;
    }
}

void StreamConfigurationMap::appendAvailableStreamStallDurations(
        const camera_metadata_ro_entry& entry) {
    for (size_t i = 0; i < entry.count; i+= kStreamConfigurationSize) {
        auto format = static_cast<android_pixel_format_t> (
                entry.data.i64[i + kStreamFormatOffset]);
        uint32_t width = entry.data.i64[i + kStreamWidthOffset];
        uint32_t height = entry.data.i64[i + kStreamHeightOffset];
        nsecs_t duration = entry.data.i64[i + kStreamStallDurationOffset];
        auto streamConfiguration = std::make_pair(format, std::make_pair(width, height));
        mStreamStallMap[streamConfiguration] = duration;
    }
}

StreamConfigurationMap::StreamConfigurationMap(const HalCameraMetadata& chars) {
    camera_metadata_ro_entry_t entry;
    auto ret = chars.Get(ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &entry);
    if (ret != OK) {
        ALOGW("%s: ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS missing!", __FUNCTION__);
        entry.count = 0;
    }
    appendAvailableStreamConfigurations(entry);

    ret = chars.Get(ANDROID_DEPTH_AVAILABLE_DEPTH_STREAM_CONFIGURATIONS, &entry);
    if (ret == OK) {
        appendAvailableStreamConfigurations(entry);
    }

    ret = chars.Get(ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS, &entry);
    if (ret != OK) {
        ALOGW("%s: ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS missing!", __FUNCTION__);
        entry.count = 0;
    }
    appendAvailableStreamMinDurations(entry);

    ret = chars.Get(ANDROID_DEPTH_AVAILABLE_DEPTH_MIN_FRAME_DURATIONS, &entry);
    if (ret == OK) {
        appendAvailableStreamMinDurations(entry);
    }

    ret = chars.Get(ANDROID_SCALER_AVAILABLE_STALL_DURATIONS, &entry);
    if (ret != OK) {
        ALOGW("%s: ANDROID_SCALER_AVAILABLE_STALL_DURATIONS missing!", __FUNCTION__);
        entry.count = 0;
    }
    appendAvailableStreamStallDurations(entry);

    ret = chars.Get(ANDROID_DEPTH_AVAILABLE_DEPTH_STALL_DURATIONS, &entry);
    if (ret == OK) {
        appendAvailableStreamStallDurations(entry);
    }

}

}  // namespace android
