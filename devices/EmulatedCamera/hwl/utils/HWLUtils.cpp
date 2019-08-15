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

#define LOG_TAG "HWLUtils"
#include <log/log.h>
#include <map>

#include "HWLUtils.h"

namespace android {

bool hasCapability(const HalCameraMetadata* metadata, uint8_t capability) {
    if (metadata == nullptr) {
        return false;
    }

    camera_metadata_ro_entry_t entry;
    auto ret = metadata->Get(ANDROID_REQUEST_AVAILABLE_CAPABILITIES, &entry);
    if (ret != OK) {
        return false;
    }
    for (size_t i = 0; i < entry.count; i++) {
        if (entry.data.u8[i] == capability) {
            return true;
        }
    }

    return false;
}

status_t getSensorCharacteristics(const HalCameraMetadata* metadata,
        SensorCharacteristics* sensorChars /*out*/) {
    if ((metadata == nullptr) || (sensorChars == nullptr)) {
        return BAD_VALUE;
    }

    status_t ret = OK;
    camera_metadata_ro_entry_t entry;
    ret = metadata->Get(ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE, &entry);
    if ((ret != OK) || (entry.count != 2)) {
        ALOGE("%s: Invalid ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE!", __FUNCTION__);
        return BAD_VALUE;
    }
    sensorChars->width = entry.data.i32[0];
    sensorChars->height = entry.data.i32[1];

    ret = metadata->Get(ANDROID_REQUEST_MAX_NUM_OUTPUT_STREAMS, &entry);
    if ((ret != OK) || (entry.count != 3)) {
        ALOGE("%s: Invalid ANDROID_REQUEST_MAX_NUM_OUTPUT_STREAMS!", __FUNCTION__);
        return BAD_VALUE;
    }

    sensorChars->maxRawStreams = entry.data.i32[0];
    sensorChars->maxProcessedStreams = entry.data.i32[1];
    sensorChars->maxStallingStreams = entry.data.i32[2];

    if (hasCapability(metadata, ANDROID_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR)) {
        ret = metadata->Get(ANDROID_SENSOR_INFO_EXPOSURE_TIME_RANGE, &entry);
        if ((ret != OK) || (entry.count != ARRAY_SIZE(sensorChars->exposureTimeRange))) {
            ALOGE("%s: Invalid ANDROID_SENSOR_INFO_EXPOSURE_TIME_RANGE!", __FUNCTION__);
            return BAD_VALUE;
        }
        memcpy(sensorChars->exposureTimeRange, entry.data.i64,
                sizeof(sensorChars->exposureTimeRange));

        ret = metadata->Get(ANDROID_SENSOR_INFO_MAX_FRAME_DURATION, &entry);
        if ((ret != OK) || (entry.count != 1)) {
            ALOGE("%s: Invalid ANDROID_SENSOR_INFO_MAX_FRAME_DURATION!", __FUNCTION__);
            return BAD_VALUE;
        }
        sensorChars->frameDurationRange[1] = entry.data.i64[0];
        sensorChars->frameDurationRange[0] = EmulatedSensor::kSupportedFrameDurationRange[0];

        ret = metadata->Get(ANDROID_SENSOR_INFO_SENSITIVITY_RANGE, &entry);
        if ((ret != OK) || (entry.count != ARRAY_SIZE(sensorChars->sensitivityRange))) {
            ALOGE("%s: Invalid ANDROID_SENSOR_INFO_SENSITIVITY_RANGE!", __FUNCTION__);
            return BAD_VALUE;
        }
        memcpy(sensorChars->sensitivityRange, entry.data.i64,
                sizeof(sensorChars->sensitivityRange));
    } else {
        memcpy(sensorChars->exposureTimeRange, EmulatedSensor::kSupportedExposureTimeRange,
                sizeof(sensorChars->exposureTimeRange));
        memcpy(sensorChars->frameDurationRange, EmulatedSensor::kSupportedFrameDurationRange,
                sizeof(sensorChars->frameDurationRange));
        memcpy(sensorChars->sensitivityRange, EmulatedSensor::kSupportedSensitivityRange,
                sizeof(sensorChars->sensitivityRange));
    }

    if (hasCapability(metadata, ANDROID_REQUEST_AVAILABLE_CAPABILITIES_RAW)) {
        ret = metadata->Get(ANDROID_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT, &entry);
        if ((ret != OK) || (entry.count != 1)) {
            ALOGE("%s: Invalid ANDROID_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT!", __FUNCTION__);
            return BAD_VALUE;
        }

        sensorChars->colorArangement =
            static_cast<camera_metadata_enum_android_sensor_info_color_filter_arrangement> (
                    entry.data.u8[0]);

        ret = metadata->Get(ANDROID_SENSOR_INFO_WHITE_LEVEL, &entry);
        if ((ret != OK) || (entry.count != 1)) {
            ALOGE("%s: Invalid ANDROID_SENSOR_INFO_WHITE_LEVEL!", __FUNCTION__);
            return BAD_VALUE;
        }
        sensorChars->maxRawValue = entry.data.i32[0];

        ret = metadata->Get(ANDROID_SENSOR_BLACK_LEVEL_PATTERN, &entry);
        if ((ret != OK) || (entry.count != ARRAY_SIZE(sensorChars->blackLevelPattern))) {
            ALOGE("%s: Invalid ANDROID_SENSOR_BLACK_LEVEL_PATTERN!", __FUNCTION__);
            return BAD_VALUE;
        }

        memcpy(sensorChars->blackLevelPattern, entry.data.i32,
                sizeof(sensorChars->blackLevelPattern));

        ret = metadata->Get(ANDROID_LENS_INFO_SHADING_MAP_SIZE, &entry);
        if ((ret == OK) && (entry.count == 2)) {
            sensorChars->lensShadingMapSize[0] = entry.data.i32[0];
            sensorChars->lensShadingMapSize[1] = entry.data.i32[1];
        } else {
            ALOGE("%s: No available shading map size!", __FUNCTION__);
            return BAD_VALUE;
        }

        ret = metadata->Get(ANDROID_SENSOR_COLOR_TRANSFORM1, &entry);
        if ((ret != OK) || (entry.count != (3 * 3))) { // 3x3 rational matrix
            ALOGE("%s: Invalid ANDROID_SENSOR_COLOR_TRANSFORM1!", __FUNCTION__);
            return BAD_VALUE;
        }

        sensorChars->colorFilter.rX = RAT_TO_FLOAT(entry.data.r[0]);
        sensorChars->colorFilter.rY = RAT_TO_FLOAT(entry.data.r[1]);
        sensorChars->colorFilter.rZ = RAT_TO_FLOAT(entry.data.r[2]);
        sensorChars->colorFilter.grX = RAT_TO_FLOAT(entry.data.r[3]);
        sensorChars->colorFilter.grY = RAT_TO_FLOAT(entry.data.r[4]);
        sensorChars->colorFilter.grZ = RAT_TO_FLOAT(entry.data.r[5]);
        sensorChars->colorFilter.gbX = RAT_TO_FLOAT(entry.data.r[3]);
        sensorChars->colorFilter.gbY = RAT_TO_FLOAT(entry.data.r[4]);
        sensorChars->colorFilter.gbZ = RAT_TO_FLOAT(entry.data.r[5]);
        sensorChars->colorFilter.bX = RAT_TO_FLOAT(entry.data.r[6]);
        sensorChars->colorFilter.bY = RAT_TO_FLOAT(entry.data.r[7]);
        sensorChars->colorFilter.bZ = RAT_TO_FLOAT(entry.data.r[8]);
    } else {
        sensorChars->colorArangement =
            static_cast<camera_metadata_enum_android_sensor_info_color_filter_arrangement> (
                    EmulatedSensor::kSupportedColorFilterArrangement);
        sensorChars->maxRawValue = EmulatedSensor::kDefaultMaxRawValue;
        memcpy(sensorChars->blackLevelPattern, EmulatedSensor::kDefaultBlackLevelPattern,
                sizeof(sensorChars->blackLevelPattern));
    }

    if (hasCapability(metadata, ANDROID_REQUEST_AVAILABLE_CAPABILITIES_PRIVATE_REPROCESSING) ||
            hasCapability(metadata, ANDROID_REQUEST_AVAILABLE_CAPABILITIES_YUV_REPROCESSING))  {
        ret = metadata->Get(ANDROID_REQUEST_MAX_NUM_INPUT_STREAMS, &entry);
        if ((ret != OK) || (entry.count != 1)) {
            ALOGE("%s: Invalid ANDROID_REQUEST_MAX_NUM_INPUT_STREAMS!", __FUNCTION__);
            return BAD_VALUE;
        }

        sensorChars->maxInputStreams = entry.data.i32[0];
    }


    ret = metadata->Get(ANDROID_REQUEST_PIPELINE_MAX_DEPTH, &entry);
    if ((ret == OK) && (entry.count == 1)) {
        if (entry.data.u8[0] == 0) {
            ALOGE("%s: Maximum request pipeline must have a non zero value!", __FUNCTION__);
            return BAD_VALUE;
        }
        sensorChars->maxPipelineDepth = entry.data.u8[0];
    } else {
        ALOGE("%s: Maximum request pipeline depth absent!", __FUNCTION__);
        return BAD_VALUE;
    }

    return ret;
}

}  // namespace android
