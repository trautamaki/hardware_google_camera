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

#define LOG_TAG "EmulatedRequestState"
#define ATRACE_TAG ATRACE_TAG_CAMERA

#include "EmulatedRequestProcessor.h"
#include "EmulatedRequestState.h"
#include <inttypes.h>
#include <log/log.h>
#include <utils/HWLUtils.h>

namespace android {

using google_camera_hal::HwlPipelineResult;

const std::set<uint8_t> EmulatedRequestState::kSupportedCapabilites = {
    ANDROID_REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE,
    ANDROID_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR,
    ANDROID_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_POST_PROCESSING,
    ANDROID_REQUEST_AVAILABLE_CAPABILITIES_RAW,
    ANDROID_REQUEST_AVAILABLE_CAPABILITIES_READ_SENSOR_SETTINGS,
    //TODO: Support more capabilities out-of-the box
};

const std::set<uint8_t> EmulatedRequestState::kSupportedHWLevels = {
    ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_LIMITED,
    ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_FULL,
    //TODO: Support more hw levels out-of-the box
};

template<typename T>
T getClosestValue(T val, T min, T max) {
    if ((min > max) || ((val >= min) && (val <= max))) {
        return val;
    } else if (val > max) {
        return max;
    } else {
        return min;
    }
}

status_t EmulatedRequestState::doFakeAE() {
    camera_metadata_ro_entry_t entry;
    auto ret = mRequestSettings->Get(ANDROID_CONTROL_AE_LOCK, &entry);
    if ((ret == OK) && (entry.count == 1)) {
        mAELock = entry.data.u8[0];
    } else {
        mAELock = ANDROID_CONTROL_AE_LOCK_OFF;
    }

    if (mAELock == ANDROID_CONTROL_AE_LOCK_ON) {
        mAEState = ANDROID_CONTROL_AE_STATE_LOCKED;
        return OK;
    }

    FPSRange fpsRange;
    ret = mRequestSettings->Get(ANDROID_CONTROL_AE_TARGET_FPS_RANGE, &entry);
    if ((ret == OK) && (entry.count == 2)) {
        for (const auto& it : mAvailableFPSRanges) {
            if ((it.minFPS == entry.data.i32[0]) && (it.maxFPS == entry.data.i32[1])) {
                fpsRange = {entry.data.i32[0], entry.data.i32[1]};
                break;
            }
        }
        if (fpsRange.maxFPS == 0) {
            ALOGE("%s: Unsupported framerate range [%d, %d]", __FUNCTION__, entry.data.i32[0],
                    entry.data.i32[1]);
            return BAD_VALUE;
        }
    } else {
        fpsRange = *mAvailableFPSRanges.begin();
    }

    ret = mRequestSettings->Get(ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER, &entry);
    if ((ret == OK) && (entry.count == 1)) {
        mAETrigger = entry.data.u8[0];
    } else {
        mAETrigger = ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_IDLE;
    }

    nsecs_t minFrameDuration = getClosestValue(s2ns(1.f / fpsRange.maxFPS),
            EmulatedSensor::kSupportedFrameDurationRange[0], mSensorMaxFrameDuration);
    nsecs_t maxFrameDuration = getClosestValue(s2ns(1.f / fpsRange.minFPS),
            EmulatedSensor::kSupportedFrameDurationRange[0], mSensorMaxFrameDuration);
    mSensorFrameDuration = (maxFrameDuration + minFrameDuration) / 2;
    // Use a different AE target exposure for face priority mode
    if (mSceneMode == ANDROID_CONTROL_SCENE_MODE_FACE_PRIORITY) {
        mAETargetExposureTime = getClosestValue(mSensorFrameDuration,
                mSensorExposureTimeRange.first, mSensorExposureTimeRange.second);
    } else {
        mAETargetExposureTime = getClosestValue(mSensorFrameDuration / 2,
                mSensorExposureTimeRange.first, mSensorExposureTimeRange.second);
    }

    if ((mAETrigger == ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_START) ||
            (mAEState == ANDROID_CONTROL_AE_STATE_PRECAPTURE)) {
        if (mAEState != ANDROID_CONTROL_AE_STATE_PRECAPTURE) {
            mAEFrameCounter = 0;
        }
        if (((mAEFrameCounter > kAEPrecaptureMinFrames) &&
                    (mAETargetExposureTime - mSensorExposureTime) < mAETargetExposureTime / 10) ||
                (mAETrigger == ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_CANCEL)) {
            // Done with precapture
            mAEFrameCounter = 0;
            mAEState = ANDROID_CONTROL_AE_STATE_CONVERGED;
            mAETrigger = ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_IDLE;
        } else {
            // Converge some more
            mSensorExposureTime +=
                (mAETargetExposureTime - mSensorExposureTime) * kExposureTrackRate;
            mAEFrameCounter++;
            mAEState = ANDROID_CONTROL_AE_STATE_PRECAPTURE;
        }
    } else {
        switch (mAEState) {
            case ANDROID_CONTROL_AE_STATE_INACTIVE:
                mAEState = ANDROID_CONTROL_AE_STATE_SEARCHING;
                break;
            case ANDROID_CONTROL_AE_STATE_CONVERGED:
                mAEFrameCounter++;
                if (mAEFrameCounter > kStableAeMaxFrames) {
                    float exposureStep = ((double)rand() / RAND_MAX) *
                        (kExposureWanderMax - kExposureWanderMin) +
                        kExposureWanderMin;
                    mAETargetExposureTime = getClosestValue(
                            static_cast<nsecs_t> (mAETargetExposureTime *
                                std::pow(2, exposureStep)),
                            mSensorExposureTimeRange.first, mSensorExposureTimeRange.second);
                    mAEState = ANDROID_CONTROL_AE_STATE_SEARCHING;
                }
                break;
            case ANDROID_CONTROL_AE_STATE_SEARCHING:
                mSensorExposureTime += (mAETargetExposureTime - mSensorExposureTime) *
                    kExposureTrackRate;
                if (abs(mAETargetExposureTime - mSensorExposureTime) <
                        mAETargetExposureTime / 10) {
                    // Close enough
                    mAEState = ANDROID_CONTROL_AE_STATE_CONVERGED;
                    mAEFrameCounter = 0;
                }
                break;
            case ANDROID_CONTROL_AE_STATE_LOCKED:
                mAEState = ANDROID_CONTROL_AE_STATE_CONVERGED;
                mAEFrameCounter = 0;
                break;
            default:
                ALOGE("%s: Unexpected AE state %d!", __FUNCTION__, mAEState);
                return INVALID_OPERATION;
        }
    }

    return OK;
}

status_t EmulatedRequestState::processAE() {
    camera_metadata_ro_entry_t entry;
    if (((mAEMode == ANDROID_CONTROL_AE_MODE_OFF) || (mControlMode == ANDROID_CONTROL_MODE_OFF)) &&
            mSupportsManualSensor) {
        auto ret = mRequestSettings->Get(ANDROID_SENSOR_EXPOSURE_TIME, &entry);
        if ((ret == OK) && (entry.count == 1)) {
            if ((entry.data.i64[0] >= mSensorExposureTimeRange.first) &&
                    (entry.data.i64[0] <= mSensorExposureTimeRange.second)) {
                mSensorExposureTime = entry.data.i64[0];
            } else {
                ALOGE("%s: Sensor exposure time: %" PRId64 " not within supported range[%"
                        PRId64 ", %" PRId64 "]", __FUNCTION__, entry.data.i64[0],
                            mSensorExposureTimeRange.first, mSensorExposureTimeRange.second);
                return BAD_VALUE;
            }
        }

        ret = mRequestSettings->Get(ANDROID_SENSOR_FRAME_DURATION, &entry);
        if ((ret == OK) && (entry.count == 1)) {
            if ((entry.data.i64[0] >= EmulatedSensor::kSupportedFrameDurationRange[0]) &&
                    (entry.data.i64[0] <= mSensorMaxFrameDuration)) {
                mSensorFrameDuration = entry.data.i64[0];
            } else {
                ALOGE("%s: Sensor frame duration : %" PRId64 " not within supported range[%"
                        PRId64 ", %" PRId64 "]", __FUNCTION__, entry.data.i64[0],
                            EmulatedSensor::kSupportedFrameDurationRange[0],
                            mSensorMaxFrameDuration);
                return BAD_VALUE;
            }
        }

        ret = mRequestSettings->Get(ANDROID_SENSOR_SENSITIVITY, &entry);
        if ((ret == OK) && (entry.count == 1)) {
            if ((entry.data.i32[0] >= mSensorSensitivityRange.first) &&
                    (entry.data.i32[0] <= mSensorSensitivityRange.second)) {
                mSensorSensitivity = entry.data.i32[0];
            } else {
                ALOGE("%s: Sensor sensitivity: %d not within supported range[%d, %d]", __FUNCTION__,
                        entry.data.i32[0], mSensorSensitivityRange.first,
                        mSensorSensitivityRange.second);
                return BAD_VALUE;
            }
        }
        mAEState = ANDROID_CONTROL_AE_STATE_INACTIVE;
    } else if (mIsBackwardCompatible && (mAEMode == ANDROID_CONTROL_AE_MODE_ON)) {
        return doFakeAE();
    } else {
        ALOGI("%s: No emulation for AE mode: %d using previous sensor settings!", __FUNCTION__,
                mAEMode);
    }

    return OK;
}

status_t EmulatedRequestState::initializeSensorSettings(
        std::unique_ptr<HalCameraMetadata> requestSettings,
        EmulatedSensor::SensorSettings *sensorSettings/*out*/) {
    if ((sensorSettings == nullptr) || (requestSettings.get() == nullptr)) {
        return BAD_VALUE;
    }

    std::lock_guard<std::mutex> lock(mRequestStateMutex);
    mRequestSettings = std::move(requestSettings);
    camera_metadata_ro_entry_t entry;
    auto ret = mRequestSettings->Get(ANDROID_CONTROL_MODE, &entry);
    if ((ret == OK) && (entry.count == 1)) {
        if (mAvailableControlModes.find(entry.data.u8[0]) != mAvailableControlModes.end()) {
            mControlMode = entry.data.u8[0];
        } else {
            ALOGE("%s: Control mode: %d not supported!", __FUNCTION__, entry.data.u8[0]);
            return BAD_VALUE;
        }
    }

    ret = mRequestSettings->Get(ANDROID_CONTROL_SCENE_MODE, &entry);
    if ((ret == OK) && (entry.count == 1)) {
        // Disabled scene is not expected to be among the available scene list
        if ((entry.data.u8[0] == ANDROID_CONTROL_SCENE_MODE_DISABLED) ||
                (mAvailableScenes.find(entry.data.u8[0]) != mAvailableScenes.end())) {
            mSceneMode = entry.data.u8[0];
        } else {
            ALOGE("%s: Scene mode: %d not supported!", __FUNCTION__, entry.data.u8[0]);
            return BAD_VALUE;
        }
    }

    // 3A modes are active in case the scene is disabled or set to face priority or the control
    // mode is not using scenes
    if ((mSceneMode == ANDROID_CONTROL_SCENE_MODE_DISABLED) ||
            (mSceneMode == ANDROID_CONTROL_SCENE_MODE_FACE_PRIORITY) ||
            (mControlMode != ANDROID_CONTROL_MODE_USE_SCENE_MODE)) {
        ret = mRequestSettings->Get(ANDROID_CONTROL_AE_MODE, &entry);
        if ((ret == OK) && (entry.count == 1)) {
            if (mAvailableAEModes.find(entry.data.u8[0]) != mAvailableAEModes.end()) {
                mAEMode = entry.data.u8[0];
            } else {
                ALOGE("%s: AE mode: %d not supported!", __FUNCTION__, entry.data.u8[0]);
                return BAD_VALUE;
            }
        }

        ret = mRequestSettings->Get(ANDROID_CONTROL_AWB_MODE, &entry);
        if ((ret == OK) && (entry.count == 1)) {
            if (mAvailableAWBModes.find(entry.data.u8[0]) != mAvailableAWBModes.end()) {
                mAWBMode = entry.data.u8[0];
            } else {
                ALOGE("%s: AWB mode: %d not supported!", __FUNCTION__, entry.data.u8[0]);
                return BAD_VALUE;
            }
        }

        ret = mRequestSettings->Get(ANDROID_CONTROL_AF_MODE, &entry);
        if ((ret == OK) && (entry.count == 1)) {
            if (mAvailableAFModes.find(entry.data.u8[0]) != mAvailableAFModes.end()) {
                mAFMode = entry.data.u8[0];
            } else {
                ALOGE("%s: AF mode: %d not supported!", __FUNCTION__, entry.data.u8[0]);
                return BAD_VALUE;
            }
        }
    } else {
        auto it = mSceneOverrides.find(mSceneMode);
        if (it != mSceneOverrides.end()) {
            mAEMode = it->second.aeMode;
            mAWBMode = it->second.awbMode;
            mAFMode = it->second.afMode;
        } else {
            ALOGW("%s: Scene %d has no scene overrides using the currently active 3A modes!",
                    __FUNCTION__, mSceneMode);
        }
    }

    ret = processAE();
    if (ret != OK) {
        return ret;
    }

    sensorSettings->exposureTime = mSensorExposureTime;
    sensorSettings->frameDuration = mSensorFrameDuration;
    sensorSettings->gain = mSensorSensitivity;

    return OK;
}

std::unique_ptr<HwlPipelineResult> EmulatedRequestState::initializeResult(
        const PendingRequest& request, uint32_t pipelineId, uint32_t frameNumber) {
    std::lock_guard<std::mutex> lock(mRequestStateMutex);
    auto result = std::make_unique<HwlPipelineResult>();
    result->camera_id = mCameraId;
    result->pipeline_id = pipelineId;
    result->frame_number = frameNumber;
    result->result_metadata = request.settings != nullptr ?
            HalCameraMetadata::Clone(request.settings.get()) : HalCameraMetadata::Create(1, 10);
    result->result_metadata->Set(ANDROID_REQUEST_PIPELINE_DEPTH, &mMaxPipelineDepth, 1);
    result->result_metadata->Set(ANDROID_CONTROL_MODE, &mControlMode, 1);
    result->result_metadata->Set(ANDROID_CONTROL_AF_MODE, &mAFMode, 1);
    result->result_metadata->Set(ANDROID_CONTROL_AF_STATE, &mAFState, 1);
    result->result_metadata->Set(ANDROID_CONTROL_AWB_MODE, &mAWBMode, 1);
    result->result_metadata->Set(ANDROID_CONTROL_AWB_STATE, &mAWBState, 1);
    result->result_metadata->Set(ANDROID_CONTROL_AE_MODE, &mAEMode, 1);
    result->result_metadata->Set(ANDROID_CONTROL_AE_STATE, &mAEState, 1);
    result->result_metadata->Set(ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER, &mAETrigger, 1);
    int32_t fpsRange[] = {mAETargetFPS.minFPS, mAETargetFPS.maxFPS};
    result->result_metadata->Set(ANDROID_CONTROL_AE_TARGET_FPS_RANGE, fpsRange,
            ARRAY_SIZE(fpsRange));
    if (mAELockAvailable && mReportAELock) {
        result->result_metadata->Set(ANDROID_CONTROL_AE_LOCK, &mAELock, 1);
    }
    if (mScenesSupported) {
        result->result_metadata->Set(ANDROID_CONTROL_SCENE_MODE, &mSceneMode, 1);
    }
    if (mReportSensorSettings) {
        result->result_metadata->Set(ANDROID_SENSOR_EXPOSURE_TIME, &mSensorExposureTime, 1);
        result->result_metadata->Set(ANDROID_SENSOR_FRAME_DURATION, &mSensorFrameDuration, 1);
        result->result_metadata->Set(ANDROID_SENSOR_SENSITIVITY, &mSensorSensitivity, 1);
    }
    if (mReportFlashState) {
        result->result_metadata->Set(ANDROID_FLASH_STATE, &mFlashState, 1);
    }
    result->input_buffers = request.inputBuffers;
    result->partial_result = mPartialResultCount;

    return result;
}

bool EmulatedRequestState::supportsCapability(uint8_t cap) {
    return mAvailableCapabilites.find(cap) != mAvailableCapabilites.end();
}

status_t EmulatedRequestState::initializeSensorDefaults() {
    camera_metadata_ro_entry_t entry;
    auto ret = mStaticMetadata->Get(ANDROID_SENSOR_INFO_SENSITIVITY_RANGE, &entry);
    if ((ret == OK) && (entry.count == 2)) {
        mSensorSensitivityRange = std::make_pair(entry.data.i32[0], entry.data.i32[1]);
    } else if (!mSupportsManualSensor) {
        mSensorSensitivityRange = std::make_pair(EmulatedSensor::kSupportedSensitivityRange[0],
                EmulatedSensor::kSupportedSensitivityRange[1]);
    } else {
        ALOGE("%s: Manual sensor devices must advertise sensor sensitivity range!", __FUNCTION__);
        return BAD_VALUE;
    }

    ret = mStaticMetadata->Get(ANDROID_SENSOR_INFO_EXPOSURE_TIME_RANGE, &entry);
    if ((ret == OK) && (entry.count == 2)) {
        mSensorExposureTimeRange = std::make_pair(entry.data.i64[0], entry.data.i64[1]);
    } else if (!mSupportsManualSensor) {
        mSensorExposureTimeRange = std::make_pair(EmulatedSensor::kSupportedExposureTimeRange[0],
                EmulatedSensor::kSupportedExposureTimeRange[1]);
    } else {
        ALOGE("%s: Manual sensor devices must advertise sensor exposure time range!", __FUNCTION__);
        return BAD_VALUE;
    }

    ret = mStaticMetadata->Get(ANDROID_SENSOR_INFO_MAX_FRAME_DURATION, &entry);
    if ((ret == OK) && (entry.count == 1)) {
        mSensorMaxFrameDuration = entry.data.i64[0];
    } else if (!mSupportsManualSensor) {
        mSensorMaxFrameDuration = EmulatedSensor::kSupportedFrameDurationRange[1];
    } else {
        ALOGE("%s: Manual sensor devices must advertise sensor max frame duration!", __FUNCTION__);
        return BAD_VALUE;
    }

    if (mSupportsManualSensor) {
        if (mAvailableRequests.find(ANDROID_SENSOR_SENSITIVITY) == mAvailableRequests.end()) {
            ALOGE("%s: Sensor sensitivity must be configurable on manual sensor devices!",
                    __FUNCTION__);
            return BAD_VALUE;
        }

        if (mAvailableRequests.find(ANDROID_SENSOR_EXPOSURE_TIME) == mAvailableRequests.end()) {
            ALOGE("%s: Sensor exposure time must be configurable on manual sensor devices!",
                    __FUNCTION__);
            return BAD_VALUE;
        }

        if (mAvailableRequests.find(ANDROID_SENSOR_FRAME_DURATION) == mAvailableRequests.end()) {
            ALOGE("%s: Sensor frame duration must be configurable on manual sensor devices!",
                    __FUNCTION__);
            return BAD_VALUE;
        }
    }

    if (mAvailableResults.find(ANDROID_SENSOR_TIMESTAMP) == mAvailableRequests.end()) {
        ALOGE("%s: Sensor timestamp must always be part of the results!", __FUNCTION__);
        return BAD_VALUE;
    }

    if (mReportSensorSettings) {
        if (mAvailableResults.find(ANDROID_SENSOR_SENSITIVITY) == mAvailableRequests.end()) {
            ALOGE("%s: Sensor sensitivity must always be part of the results on devices supporting"
                    " sensor setting reads!", __FUNCTION__);
            return BAD_VALUE;
        }

        if (mAvailableResults.find(ANDROID_SENSOR_EXPOSURE_TIME) == mAvailableRequests.end()) {
            ALOGE("%s: Sensor exposure time must always be part of the results on devices"
                    "supporting sensor setting reads!", __FUNCTION__);
            return BAD_VALUE;
        }

        if (mAvailableResults.find(ANDROID_SENSOR_FRAME_DURATION) == mAvailableRequests.end()) {
            ALOGE("%s: Sensor frame duration must always be part of the results on devices"
                    "supporting sensor setting reads!", __FUNCTION__);
            return BAD_VALUE;
        }
    }

    mSensorExposureTime = getClosestValue(EmulatedSensor::kDefaultExposureTime,
            mSensorExposureTimeRange.first, mSensorExposureTimeRange.second);
    mSensorFrameDuration = getClosestValue(EmulatedSensor::kDefaultFrameDuration,
            EmulatedSensor::kSupportedFrameDurationRange[0], mSensorMaxFrameDuration);
    mSensorSensitivity = getClosestValue(EmulatedSensor::kDefaultSensitivity,
            mSensorSensitivityRange.first, mSensorSensitivityRange.second);

    auto manualIdx = static_cast<size_t>(RequestTemplate::kManual);
    if (mDefaultRequests[manualIdx].get() != nullptr) {
        mDefaultRequests[manualIdx]->Set(ANDROID_SENSOR_EXPOSURE_TIME, &mSensorExposureTime, 1);
        mDefaultRequests[manualIdx]->Set(ANDROID_SENSOR_FRAME_DURATION, &mSensorFrameDuration, 1);
        mDefaultRequests[manualIdx]->Set(ANDROID_SENSOR_SENSITIVITY, &mSensorSensitivity, 1);
    }

    return OK;
}

status_t EmulatedRequestState::initializeControlSceneDefaults() {
    camera_metadata_ro_entry_t entry;
    auto ret = mStaticMetadata->Get(ANDROID_CONTROL_AVAILABLE_SCENE_MODES, &entry);
    if (ret == OK) {
        mAvailableScenes.insert(entry.data.u8, entry.data.u8 + entry.count);
    } else {
        ALOGE("%s: No available scene modes!", __FUNCTION__);
        return BAD_VALUE;
    }

    if ((entry.count == 1) && (entry.data.u8[0] == ANDROID_CONTROL_SCENE_MODE_DISABLED)) {
        mScenesSupported = false;
        return OK;
    } else {
        mScenesSupported = true;
    }

    if (mAvailableRequests.find(ANDROID_CONTROL_SCENE_MODE) == mAvailableRequests.end()) {
        ALOGE("%s: Scene mode cannot be set!", __FUNCTION__);
        return BAD_VALUE;
    }

    if (mAvailableResults.find(ANDROID_CONTROL_SCENE_MODE) == mAvailableResults.end()) {
        ALOGE("%s: Scene mode cannot be reported!", __FUNCTION__);
        return BAD_VALUE;
    }

    camera_metadata_ro_entry_t overridesEntry;
    ret = mStaticMetadata->Get(ANDROID_CONTROL_SCENE_MODE_OVERRIDES, &overridesEntry);
    if ((ret == OK) && ((overridesEntry.count / 3) == mAvailableScenes.size()) &&
            ((overridesEntry.count % 3) == 0)) {
        for (size_t i = 0; i < entry.count; i += 3) {
            SceneOverride scene(overridesEntry.data.u8[i], overridesEntry.data.u8[i + 1],
                    overridesEntry.data.u8[i + 2]);
            if (mAvailableAEModes.find(scene.aeMode) == mAvailableAEModes.end()) {
                ALOGE("%s: AE scene mode override: %d not supported!", __FUNCTION__, scene.aeMode);
                return BAD_VALUE;
            }
            if (mAvailableAWBModes.find(scene.awbMode) == mAvailableAWBModes.end()) {
                ALOGE("%s: AWB scene mode override: %d not supported!", __FUNCTION__,
                        scene.awbMode);
                return BAD_VALUE;
            }
            if (mAvailableAFModes.find(scene.afMode) == mAvailableAFModes.end()) {
                ALOGE("%s: AF scene mode override: %d not supported!", __FUNCTION__,
                        scene.afMode);
                return BAD_VALUE;
            }
            mSceneOverrides.emplace(entry.data.u8[i], scene);
        }
    } else {
        ALOGE("%s: No available scene overrides!", __FUNCTION__);
        return BAD_VALUE;
    }

    return OK;
}

status_t EmulatedRequestState::initializeControlAFDefaults() {
    camera_metadata_ro_entry_t entry;
    auto ret = mStaticMetadata->Get(ANDROID_CONTROL_AF_AVAILABLE_MODES, &entry);
    if (ret == OK) {
        mAvailableAFModes.insert(entry.data.u8, entry.data.u8 + entry.count);
    } else {
        ALOGE("%s: No available AF modes!", __FUNCTION__);
        return BAD_VALUE;
    }
    // Off mode must always be present
    if (mAvailableAFModes.find(ANDROID_CONTROL_AF_MODE_OFF) == mAvailableAFModes.end()) {
        ALOGE("%s: AF off control mode must always be present!", __FUNCTION__);
        return BAD_VALUE;
    }

    if (mAvailableResults.find(ANDROID_CONTROL_AF_MODE) == mAvailableResults.end()) {
        ALOGE("%s: AF mode must be reported!", __FUNCTION__);
        return BAD_VALUE;
    }

    if (mAvailableResults.find(ANDROID_CONTROL_AF_STATE) == mAvailableResults.end()) {
        ALOGE("%s: AF state must be reported!", __FUNCTION__);
        return BAD_VALUE;
    }

    return OK;
}
status_t EmulatedRequestState::initializeControlAWBDefaults() {
    camera_metadata_ro_entry_t entry;
    auto ret = mStaticMetadata->Get(ANDROID_CONTROL_AWB_AVAILABLE_MODES, &entry);
    if (ret == OK) {
        mAvailableAWBModes.insert(entry.data.u8, entry.data.u8 + entry.count);
    } else {
        ALOGE("%s: No available AWB modes!", __FUNCTION__);
        return BAD_VALUE;
    }
    // Auto mode must always be present
    if (mAvailableAWBModes.find(ANDROID_CONTROL_AWB_MODE_AUTO) == mAvailableAWBModes.end()) {
        ALOGE("%s: AWB auto control mode must always be present!", __FUNCTION__);
        return BAD_VALUE;
    }

    if (mAvailableResults.find(ANDROID_CONTROL_AWB_MODE) == mAvailableResults.end()) {
        ALOGE("%s: AWB mode must be reported!", __FUNCTION__);
        return BAD_VALUE;
    }

    if (mAvailableResults.find(ANDROID_CONTROL_AWB_STATE) == mAvailableResults.end()) {
        ALOGE("%s: AWB state must be reported!", __FUNCTION__);
        return BAD_VALUE;
    }

    return OK;
}

status_t EmulatedRequestState::initializeControlAEDefaults() {
    camera_metadata_ro_entry_t entry;
    auto ret = mStaticMetadata->Get(ANDROID_CONTROL_AE_AVAILABLE_MODES, &entry);
    if (ret == OK) {
        mAvailableAEModes.insert(entry.data.u8, entry.data.u8 + entry.count);
    } else {
        ALOGE("%s: No available AE modes!", __FUNCTION__);
        return BAD_VALUE;
    }
    // On mode must always be present
    if (mAvailableAEModes.find(ANDROID_CONTROL_AE_MODE_ON) == mAvailableAEModes.end()) {
        ALOGE("%s: AE on control mode must always be present!", __FUNCTION__);
        return BAD_VALUE;
    }

    if (mAvailableResults.find(ANDROID_CONTROL_AE_MODE) == mAvailableResults.end()) {
        ALOGE("%s: AE mode must be reported!", __FUNCTION__);
        return BAD_VALUE;
    }

    if (mAvailableResults.find(ANDROID_CONTROL_AE_STATE) == mAvailableResults.end()) {
        ALOGE("%s: AE state must be reported!", __FUNCTION__);
        return BAD_VALUE;
    }

    ret = mStaticMetadata->Get(ANDROID_CONTROL_AE_LOCK_AVAILABLE, &entry);
    if ((ret == OK) && (entry.count == 1)) {
        mAELockAvailable = entry.data.u8[0] == ANDROID_CONTROL_AE_LOCK_AVAILABLE_TRUE;
    } else {
        ALOGV("%s: No available AE lock!", __FUNCTION__);
        mAELockAvailable = false;
    }
    mReportAELock = mAvailableResults.find(ANDROID_CONTROL_AE_LOCK) != mAvailableResults.end();

    if (mSupportsManualSensor) {
        if (!mAELockAvailable) {
            ALOGE("%s: AE lock must always be available for manual sensors!", __FUNCTION__);
            return BAD_VALUE;
        }
        auto offMode = mAvailableControlModes.find(ANDROID_CONTROL_MODE_OFF);
        if (offMode == mAvailableControlModes.end()) {
            ALOGE("%s: Off control mode must always be present for manual sensors!", __FUNCTION__);
            return BAD_VALUE;
        }

        offMode = mAvailableAEModes.find(ANDROID_CONTROL_AE_MODE_OFF);
        if (offMode == mAvailableAEModes.end()) {
            ALOGE("%s: AE off control mode must always be present for manual sensors!",
                    __FUNCTION__);
            return BAD_VALUE;
        }
    }

    if (mAvailableRequests.find(ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER) ==
            mAvailableRequests.end()) {
        ALOGE("%s: Clients must be able to set AE pre-capture trigger!", __FUNCTION__);
        return BAD_VALUE;
    }

    if (mAvailableResults.find(ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER) == mAvailableResults.end()) {
        ALOGE("%s: AE pre-capture trigger must be reported!", __FUNCTION__);
        return BAD_VALUE;
    }

    ret = mStaticMetadata->Get(ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, &entry);
    if ((ret == OK) && ((entry.count % 2) == 0)) {
        mAvailableFPSRanges.reserve(entry.count / 2);
        for (size_t i = 0; i < entry.count; i += 2) {
            FPSRange range(entry.data.i32[0], entry.data.i32[1]);
            if (range.minFPS > range.maxFPS) {
                ALOGE("%s: Mininum framerate: %d bigger than maximum framerate: %d", __FUNCTION__,
                        range.minFPS, range.maxFPS);
                return BAD_VALUE;
            }
            mAvailableFPSRanges.push_back(range);
        }
    } else {
        ALOGE("%s: No available framerate ranges!", __FUNCTION__);
        return BAD_VALUE;
    }

    if (mAvailableRequests.find(ANDROID_CONTROL_AE_TARGET_FPS_RANGE) == mAvailableRequests.end()) {
        ALOGE("%s: Clients must be able to set the target framerate range!", __FUNCTION__);
        return BAD_VALUE;
    }

    if (mAvailableResults.find(ANDROID_CONTROL_AE_TARGET_FPS_RANGE) == mAvailableResults.end()) {
        ALOGE("%s: Target framerate must be reported!", __FUNCTION__);
        return BAD_VALUE;
    }

    return OK;
}

status_t EmulatedRequestState::initializeControlDefaults() {
    camera_metadata_ro_entry_t entry;
    auto ret = mStaticMetadata->Get(ANDROID_CONTROL_AVAILABLE_MODES, &entry);
    if (ret == OK) {
        mAvailableControlModes.insert(entry.data.u8, entry.data.u8 + entry.count);
    } else {
        ALOGE("%s: No available control modes!", __FUNCTION__);
        return BAD_VALUE;
    }

    // Auto mode must always be present
    if (mAvailableControlModes.find(ANDROID_CONTROL_MODE_AUTO) == mAvailableControlModes.end()) {
        ALOGE("%s: Auto control modes must always be present!", __FUNCTION__);
        return BAD_VALUE;
    }

    // Capture intent must always be use configurable
    if (mAvailableRequests.find(ANDROID_CONTROL_CAPTURE_INTENT) == mAvailableRequests.end()) {
        ALOGE("%s: Clients must be able to set the capture intent!", __FUNCTION__);
        return BAD_VALUE;
    }

    ret = initializeControlAEDefaults();
    if (ret != OK) {
        return ret;
    }

    ret = initializeControlAWBDefaults();
    if (ret != OK) {
        return ret;
    }

    ret = initializeControlAFDefaults();
    if (ret != OK) {
        return ret;
    }

    ret = initializeControlSceneDefaults();
    if (ret != OK) {
        return ret;
    }

    for (size_t idx = 0; idx < kTemplateCount; idx++) {
        auto templateIdx = static_cast<RequestTemplate> (idx);
        if (mDefaultRequests[idx].get() == nullptr) {
            continue;
        }

        uint8_t intent = ANDROID_CONTROL_CAPTURE_INTENT_CUSTOM;
        uint8_t controlMode, aeMode, awbMode, afMode, sceneMode;
        controlMode = ANDROID_CONTROL_MODE_AUTO;
        aeMode = ANDROID_CONTROL_AE_MODE_ON;
        awbMode = ANDROID_CONTROL_AWB_MODE_AUTO;
        afMode = ANDROID_CONTROL_AF_MODE_OFF;
        sceneMode = ANDROID_CONTROL_SCENE_MODE_DISABLED;
        uint8_t aeLock = ANDROID_CONTROL_AE_LOCK_OFF;
        int32_t aeTargetFPS [] = {mAvailableFPSRanges[0].minFPS, mAvailableFPSRanges[0].maxFPS};
        uint8_t aeTrigger = ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_IDLE;
        switch(templateIdx) {
            case RequestTemplate::kManual:
                intent = ANDROID_CONTROL_CAPTURE_INTENT_MANUAL;
                controlMode = ANDROID_CONTROL_MODE_OFF;
                aeMode = ANDROID_CONTROL_AE_MODE_OFF;
                awbMode = ANDROID_CONTROL_AWB_MODE_OFF;
                afMode = ANDROID_CONTROL_AF_MODE_OFF;
                break;
            case RequestTemplate::kPreview:
                intent = ANDROID_CONTROL_CAPTURE_INTENT_PREVIEW;
                break;
            case RequestTemplate::kStillCapture:
                intent = ANDROID_CONTROL_CAPTURE_INTENT_STILL_CAPTURE;
                break;
            case RequestTemplate::kVideoRecord:
                intent = ANDROID_CONTROL_CAPTURE_INTENT_VIDEO_RECORD;
                break;
            case RequestTemplate::kVideoSnapshot:
                intent = ANDROID_CONTROL_CAPTURE_INTENT_VIDEO_SNAPSHOT;
                break;
            default:
                //Noop
                break;
        }

        if (intent != ANDROID_CONTROL_CAPTURE_INTENT_CUSTOM) {
            mDefaultRequests[idx]->Set(ANDROID_CONTROL_CAPTURE_INTENT, &intent, 1);
            mDefaultRequests[idx]->Set(ANDROID_CONTROL_MODE, &controlMode, 1);
            mDefaultRequests[idx]->Set(ANDROID_CONTROL_AE_MODE, &aeMode, 1);
            mDefaultRequests[idx]->Set(ANDROID_CONTROL_AWB_MODE, &awbMode, 1);
            mDefaultRequests[idx]->Set(ANDROID_CONTROL_AF_MODE, &afMode, 1);
            mDefaultRequests[idx]->Set(ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER, &aeTrigger, 1);
            mDefaultRequests[idx]->Set(ANDROID_CONTROL_AE_TARGET_FPS_RANGE, aeTargetFPS,
                    ARRAY_SIZE(aeTargetFPS));
            if (mAELockAvailable) {
                mDefaultRequests[idx]->Set(ANDROID_CONTROL_AE_LOCK, &aeLock, 1);
            }
            if (mScenesSupported) {
                mDefaultRequests[idx]->Set(ANDROID_CONTROL_SCENE_MODE, &sceneMode, 1);
            }
        }
    }

    return initializeSensorDefaults();
}

status_t EmulatedRequestState::initializeFlashDefaults() {
    camera_metadata_ro_entry_t entry;
    auto ret = mStaticMetadata->Get(ANDROID_FLASH_INFO_AVAILABLE, &entry);
    if ((ret == OK) && (entry.count == 1)) {
        mIsFlashSupported = entry.data.u8[0];
    } else {
        ALOGE("%s: No available flash info defaulting to false!", __FUNCTION__);
        mIsFlashSupported = false;
    }
    mReportFlashState = mAvailableResults.find(ANDROID_FLASH_STATE) != mAvailableResults.end();

    return initializeControlDefaults();
}

status_t EmulatedRequestState::initializeInfoDefaults() {
    camera_metadata_ro_entry_t entry;
    auto ret = mStaticMetadata->Get(ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL, &entry);
    if ((ret == OK) && (entry.count == 1)) {
        if (kSupportedHWLevels.find(entry.data.u8[0]) == kSupportedCapabilites.end()) {
            ALOGE("%s: HW Level: %u not supported", __FUNCTION__, entry.data.u8[0]);
            return BAD_VALUE;
        }
    } else {
        ALOGE("%s: No available hardware level!", __FUNCTION__);
        return BAD_VALUE;
    }

    mSupportedHWLevel = entry.data.u8[0];

    return initializeFlashDefaults();
}

status_t EmulatedRequestState::initializeRequestDefaults() {
    camera_metadata_ro_entry_t entry;
    auto ret = mStaticMetadata->Get(ANDROID_REQUEST_AVAILABLE_CAPABILITIES, &entry);
    if ((ret == OK) && (entry.count > 0)) {
        for (size_t i = 0; i < entry.count; i++) {
            if (kSupportedCapabilites.find(entry.data.u8[i]) == kSupportedCapabilites.end()) {
                ALOGE("%s: Capability: %u not supported", __FUNCTION__, entry.data.u8[i]);
                return BAD_VALUE;
            }
        }
    } else {
        ALOGE("%s: No available capabilities!", __FUNCTION__);
        return BAD_VALUE;
    }
    mAvailableCapabilites.insert(entry.data.u8, entry.data.u8 + entry.count);

    ret = mStaticMetadata->Get(ANDROID_REQUEST_PIPELINE_MAX_DEPTH, &entry);
    if ((ret == OK) && (entry.count == 1)) {
        if (entry.data.u8[0] == 0) {
            ALOGE("%s: Maximum request pipeline must have a non zero value!", __FUNCTION__);
            return BAD_VALUE;
        }
    } else {
        ALOGE("%s: Maximum request pipeline depth absent!", __FUNCTION__);
        return BAD_VALUE;
    }
    mMaxPipelineDepth = entry.data.u8[0];

    ret = mStaticMetadata->Get(ANDROID_REQUEST_PARTIAL_RESULT_COUNT, &entry);
    if ((ret == OK) && (entry.count == 1)) {
        if (entry.data.i32[0] != 1) {
            ALOGW("%s: Partial results not supported!", __FUNCTION__);
        }
    }

    ret = mStaticMetadata->Get(ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS, &entry);
    if ((ret != OK) || (entry.count == 0)) {
        ALOGE("%s: No available characteristic keys!", __FUNCTION__);
        return BAD_VALUE;
    }
    mAvailableCharacteritics.insert(entry.data.i32, entry.data.i32 + entry.count);

    ret = mStaticMetadata->Get(ANDROID_REQUEST_AVAILABLE_RESULT_KEYS, &entry);
    if ((ret != OK) || (entry.count == 0)) {
        ALOGE("%s: No available result keys!", __FUNCTION__);
        return BAD_VALUE;
    }
    mAvailableResults.insert(entry.data.i32, entry.data.i32 + entry.count);

    ret = mStaticMetadata->Get(ANDROID_REQUEST_AVAILABLE_REQUEST_KEYS, &entry);
    if ((ret != OK) || (entry.count == 0)) {
        ALOGE("%s: No available request keys!", __FUNCTION__);
        return BAD_VALUE;
    }
    mAvailableRequests.insert(entry.data.i32, entry.data.i32 + entry.count);

    mSupportsManualSensor = supportsCapability(
            ANDROID_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR);
    mIsBackwardCompatible = supportsCapability(
            ANDROID_REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE);
    mReportSensorSettings = supportsCapability(
            ANDROID_REQUEST_AVAILABLE_CAPABILITIES_READ_SENSOR_SETTINGS);

    if (mSupportsManualSensor) {
        auto templateIdx = static_cast<size_t> (RequestTemplate::kManual);
        mDefaultRequests[templateIdx] = HalCameraMetadata::Create(1, 10);
    }

    if (mIsBackwardCompatible) {
        for (size_t templateIdx = 0; templateIdx < kTemplateCount; templateIdx++) {
            switch(static_cast<RequestTemplate> (templateIdx)) {
                case RequestTemplate::kPreview:
                case RequestTemplate::kStillCapture:
                case RequestTemplate::kVideoRecord:
                case RequestTemplate::kVideoSnapshot:
                    mDefaultRequests[templateIdx] = HalCameraMetadata::Create(1, 10);
                    break;
                default:
                    //Noop
                    break;
            }
        }
    }

    return initializeInfoDefaults();
}

status_t EmulatedRequestState::initialize(std::unique_ptr<HalCameraMetadata> staticMeta) {
    std::lock_guard<std::mutex> lock(mRequestStateMutex);
    mStaticMetadata = std::move(staticMeta);

    return initializeRequestDefaults();
}

status_t EmulatedRequestState::getDefaultRequest(RequestTemplate type,
        std::unique_ptr<HalCameraMetadata>* default_settings) {

    if (default_settings == nullptr) {
        ALOGE("%s default_settings is nullptr", __FUNCTION__);
        return BAD_VALUE;
    }

    std::lock_guard<std::mutex> lock(mRequestStateMutex);
    auto idx = static_cast<size_t>(type);
    if (idx >= kTemplateCount) {
        ALOGE("%s: Unexpected request type: %d", __FUNCTION__, type);
        return BAD_VALUE;
    }

    if (mDefaultRequests[idx].get() == nullptr) {
        ALOGE("%s: Unsupported request type: %d", __FUNCTION__, type);
        return BAD_VALUE;
    }

    *default_settings = HalCameraMetadata::Clone(mDefaultRequests[idx]->GetRawCameraMetadata());

    return OK;
}

}  // namespace android
