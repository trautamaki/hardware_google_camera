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
    ANDROID_REQUEST_AVAILABLE_CAPABILITIES_BURST_CAPTURE,
    ANDROID_REQUEST_AVAILABLE_CAPABILITIES_DEPTH_OUTPUT,
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

status_t EmulatedRequestState::update3AMeteringRegion(uint32_t tag,
        const HalCameraMetadata& settings, int32_t *region /*out*/) {
    if ((region == nullptr) && (tag != ANDROID_CONTROL_AE_REGIONS) &&
        (tag != ANDROID_CONTROL_AF_REGIONS) &&
        (tag != ANDROID_CONTROL_AWB_REGIONS)) {
        return BAD_VALUE;
    }

    camera_metadata_ro_entry_t entry;
    auto ret = settings.Get(ANDROID_SCALER_CROP_REGION, &entry);
    if ((ret == OK) && (entry.count > 0)) {
        int32_t cropRegion[4];
        cropRegion[0] =  entry.data.i32[0];
        cropRegion[1] =  entry.data.i32[1];
        cropRegion[2] =  entry.data.i32[2] + cropRegion[0];
        cropRegion[3] =  entry.data.i32[3] + cropRegion[1];
        ret = settings.Get(tag, &entry);
        if ((ret == OK) && (entry.count > 0)) {
            const int32_t* ARegion = entry.data.i32;
            // calculate the intersection of 3A and CROP regions
            if (ARegion[0] < cropRegion[2] && cropRegion[0] < ARegion[2] &&
                ARegion[1] < cropRegion[3] && cropRegion[1] < ARegion[3]) {
                region[0] = std::max(ARegion[0], cropRegion[0]);
                region[1] = std::max(ARegion[1], cropRegion[1]);
                region[2] = std::min(ARegion[2], cropRegion[2]);
                region[3] = std::min(ARegion[3], cropRegion[3]);
                region[4] = entry.data.i32[4];
            }
        }
    }

    return OK;
}

status_t EmulatedRequestState::compensateAE() {
    if (!mExposureCompensationSupported) {
        mSensorExposureTime = mCurrentExposureTime;
        return OK;
    }

    camera_metadata_ro_entry_t entry;
    auto ret = mRequestSettings->Get(ANDROID_CONTROL_AE_EXPOSURE_COMPENSATION, &entry);
    if ((ret == OK) && (entry.count == 1)) {
        mExposureCompensation = entry.data.i32[0];
    } else {
        ALOGW("%s: AE compensation absent from request,  re-using previous value!", __FUNCTION__);
    }

    float aeCompensation = ::powf(2,
            mExposureCompensation * ((static_cast<float>(mExposureCompensationStep.numerator) /
            mExposureCompensationStep.denominator)));

    mSensorExposureTime = getClosestValue(
            static_cast<nsecs_t>(aeCompensation * mCurrentExposureTime),
            mSensorExposureTimeRange.first, mSensorExposureTimeRange.second);

    return OK;
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

    nsecs_t minFrameDuration = getClosestValue(ms2ns(1000 / fpsRange.maxFPS),
            EmulatedSensor::kSupportedFrameDurationRange[0], mSensorMaxFrameDuration);
    nsecs_t maxFrameDuration = getClosestValue(ms2ns(1000 / fpsRange.minFPS),
            EmulatedSensor::kSupportedFrameDurationRange[0], mSensorMaxFrameDuration);
    mSensorFrameDuration = (maxFrameDuration + minFrameDuration) / 2;

    // Use a different AE target exposure for face priority mode
    if (mExposureCompensationSupported) {
        float maxAECompensation = ::powf(2,
                mExposureCompensationRange[1] * (
                (static_cast<float>(mExposureCompensationStep.numerator) /
                mExposureCompensationStep.denominator)));
        mAETargetExposureTime = getClosestValue(
                static_cast<nsecs_t> (mSensorFrameDuration / maxAECompensation),
                mSensorExposureTimeRange.first, mSensorExposureTimeRange.second);
    } else if (mSceneMode == ANDROID_CONTROL_SCENE_MODE_FACE_PRIORITY) {
        mAETargetExposureTime = getClosestValue(mSensorFrameDuration / 4,
                mSensorExposureTimeRange.first, mSensorExposureTimeRange.second);
    } else {
        mAETargetExposureTime = getClosestValue(mSensorFrameDuration / 5,
                mSensorExposureTimeRange.first, mSensorExposureTimeRange.second);
    }

    if ((mAETrigger == ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_START) ||
            (mAEState == ANDROID_CONTROL_AE_STATE_PRECAPTURE)) {
        if (mAEState != ANDROID_CONTROL_AE_STATE_PRECAPTURE) {
            mAEFrameCounter = 0;
        }

        if (mAETrigger == ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_CANCEL) {
            // Done with precapture
            mAEFrameCounter = 0;
            mAEState = ANDROID_CONTROL_AE_STATE_CONVERGED;
            mAETrigger = ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_CANCEL;
        } else if ((mAEFrameCounter > kAEPrecaptureMinFrames) &&
                ((mAETargetExposureTime - mCurrentExposureTime) < mAETargetExposureTime / 10)) {
            // Done with precapture
            mAEFrameCounter = 0;
            mAEState = ANDROID_CONTROL_AE_STATE_CONVERGED;
            mAETrigger = ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_IDLE;
        } else {
            // Converge some more
            mCurrentExposureTime +=
                (mAETargetExposureTime - mCurrentExposureTime) * kExposureTrackRate;
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
                mCurrentExposureTime += (mAETargetExposureTime - mCurrentExposureTime) *
                    kExposureTrackRate;
                if (abs(mAETargetExposureTime - mCurrentExposureTime) <
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

status_t EmulatedRequestState::processAWB() {
    if (mMaxAWBRegions > 0) {
        auto ret = update3AMeteringRegion(ANDROID_CONTROL_AWB_REGIONS, *mRequestSettings,
                mAWBMeteringRegion);
        if (ret != OK) {
            return ret;
        }

    }
    if (((mAWBMode == ANDROID_CONTROL_AWB_MODE_OFF) ||
                (mControlMode == ANDROID_CONTROL_MODE_OFF)) && mSupportsManualSensor) {
        //TODO: Add actual manual support
    } else if (mIsBackwardCompatible) {
        camera_metadata_ro_entry_t entry;
        auto ret = mRequestSettings->Get(ANDROID_CONTROL_AWB_LOCK, &entry);
        if ((ret == OK) && (entry.count == 1)) {
            mAWBLock = entry.data.u8[0];
        } else {
            mAWBLock = ANDROID_CONTROL_AWB_LOCK_OFF;
        }

        if (mAWBLock == ANDROID_CONTROL_AE_LOCK_ON) {
            mAWBState = ANDROID_CONTROL_AWB_STATE_LOCKED;
        } else {
            mAWBState = ANDROID_CONTROL_AWB_STATE_CONVERGED;
        }
    } else {
        // No color output support no need for AWB
    }

    return OK;
}

status_t EmulatedRequestState::processAF() {
    camera_metadata_ro_entry entry;

    if (mMaxAFRegions > 0) {
        auto ret = update3AMeteringRegion(ANDROID_CONTROL_AF_REGIONS, *mRequestSettings,
                mAFMeteringRegion);
        if (ret != OK) {
            return ret;
        }

    }
    if (mAFMode == ANDROID_CONTROL_AF_MODE_OFF) {
        camera_metadata_ro_entry_t entry;
        auto ret = mRequestSettings->Get(ANDROID_LENS_FOCUS_DISTANCE, &entry);
        if ((ret == OK) && (entry.count == 1)) {
            if ((entry.data.f[0] >= 0.f) && (entry.data.f[0] <= mMinimumFocusDistance)) {
                mFocusDistance = entry.data.f[0];
            } else {
                ALOGE("%s: Unsupported focus distance: %5.2f. It should be within [%5.2f, %5.2f]",
                        __FUNCTION__, entry.data.f[0], 0.f, mMinimumFocusDistance);
            }
        }

        mAFState = ANDROID_CONTROL_AF_STATE_INACTIVE;
        return OK;
    }

    auto ret = mRequestSettings->Get(ANDROID_CONTROL_AF_TRIGGER, &entry);
    if ((ret == OK) && (entry.count == 1)) {
        mAFTrigger = entry.data.u8[0];
    } else {
        mAFTrigger = ANDROID_CONTROL_AF_TRIGGER_IDLE;
    }

    /**
     * Simulate AF triggers. Transition at most 1 state per frame.
     * - Focusing always succeeds (goes into locked, or PASSIVE_SCAN).
     */

    bool afTriggerStart = false;
    switch (mAFTrigger) {
        case ANDROID_CONTROL_AF_TRIGGER_IDLE:
            break;
        case ANDROID_CONTROL_AF_TRIGGER_START:
            afTriggerStart = true;
            break;
        case ANDROID_CONTROL_AF_TRIGGER_CANCEL:
            // Cancel trigger always transitions into INACTIVE
            mAFState = ANDROID_CONTROL_AF_STATE_INACTIVE;

            // Stay in 'inactive' until at least next frame
            return OK;
        default:
            ALOGE("%s: Unknown af trigger value %d", __FUNCTION__, mAFTrigger);
            return BAD_VALUE;
    }

    // If we get down here, we're either in an autofocus mode
    //  or in a continuous focus mode (and no other modes)
    switch (mAFState) {
        case ANDROID_CONTROL_AF_STATE_INACTIVE:
            if (afTriggerStart) {
                switch (mAFMode) {
                    case ANDROID_CONTROL_AF_MODE_AUTO:
                        // fall-through
                    case ANDROID_CONTROL_AF_MODE_MACRO:
                        mAFState = ANDROID_CONTROL_AF_STATE_ACTIVE_SCAN;
                        break;
                    case ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO:
                        // fall-through
                    case ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE:
                        mAFState = ANDROID_CONTROL_AF_STATE_NOT_FOCUSED_LOCKED;
                        break;
                }
            } else {
                // At least one frame stays in INACTIVE
                if (!mAFModeChanged) {
                    switch (mAFMode) {
                        case ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO:
                            // fall-through
                        case ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE:
                            mAFState = ANDROID_CONTROL_AF_STATE_PASSIVE_SCAN;
                            break;
                    }
                }
            }
            break;
        case ANDROID_CONTROL_AF_STATE_PASSIVE_SCAN:
            /**
             * When the AF trigger is activated, the algorithm should finish
             * its PASSIVE_SCAN if active, and then transition into AF_FOCUSED
             * or AF_NOT_FOCUSED as appropriate
             */
            if (afTriggerStart) {
                // Randomly transition to focused or not focused
                if (rand() % 3) {
                    mAFState = ANDROID_CONTROL_AF_STATE_FOCUSED_LOCKED;
                } else {
                    mAFState = ANDROID_CONTROL_AF_STATE_NOT_FOCUSED_LOCKED;
                }
            }
            /**
             * When the AF trigger is not involved, the AF algorithm should
             * start in INACTIVE state, and then transition into PASSIVE_SCAN
             * and PASSIVE_FOCUSED states
             */
            else {
               // Randomly transition to passive focus
                if (rand() % 3 == 0) {
                    mAFState = ANDROID_CONTROL_AF_STATE_PASSIVE_FOCUSED;
                }
            }

            break;
        case ANDROID_CONTROL_AF_STATE_PASSIVE_FOCUSED:
            if (afTriggerStart) {
                // Randomly transition to focused or not focused
                if (rand() % 3) {
                    mAFState = ANDROID_CONTROL_AF_STATE_FOCUSED_LOCKED;
                } else {
                    mAFState = ANDROID_CONTROL_AF_STATE_NOT_FOCUSED_LOCKED;
                }
            }
            // TODO: initiate passive scan (PASSIVE_SCAN)
            break;
        case ANDROID_CONTROL_AF_STATE_ACTIVE_SCAN:
            // Simulate AF sweep completing instantaneously

            // Randomly transition to focused or not focused
            if (rand() % 3) {
                mAFState = ANDROID_CONTROL_AF_STATE_FOCUSED_LOCKED;
            } else {
                mAFState = ANDROID_CONTROL_AF_STATE_NOT_FOCUSED_LOCKED;
            }
            break;
        case ANDROID_CONTROL_AF_STATE_FOCUSED_LOCKED:
            if (afTriggerStart) {
                switch (mAFMode) {
                    case ANDROID_CONTROL_AF_MODE_AUTO:
                        // fall-through
                    case ANDROID_CONTROL_AF_MODE_MACRO:
                        mAFState = ANDROID_CONTROL_AF_STATE_ACTIVE_SCAN;
                        break;
                    case ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO:
                        // fall-through
                    case ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE:
                        // continuous autofocus => trigger start has no effect
                        break;
                }
            }
            break;
        case ANDROID_CONTROL_AF_STATE_NOT_FOCUSED_LOCKED:
            if (afTriggerStart) {
                switch (mAFMode) {
                    case ANDROID_CONTROL_AF_MODE_AUTO:
                        // fall-through
                    case ANDROID_CONTROL_AF_MODE_MACRO:
                        mAFState = ANDROID_CONTROL_AF_STATE_ACTIVE_SCAN;
                        break;
                    case ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO:
                        // fall-through
                    case ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE:
                        // continuous autofocus => trigger start has no effect
                        break;
                }
            }
            break;
        default:
            ALOGE("%s: Bad af state %d", __FUNCTION__, mAFState);
    }

    return OK;
}

status_t EmulatedRequestState::processAE() {
    if (mMaxAERegions > 0) {
        auto ret = update3AMeteringRegion(ANDROID_CONTROL_AE_REGIONS, *mRequestSettings,
                mAEMeteringRegion);
        if (ret != OK) {
            ALOGE("%s: Failed updating the 3A metering regions: %d, (%s)", __FUNCTION__, ret,
                    strerror(-ret));
        }

    }

    camera_metadata_ro_entry_t entry;
    bool autoAEMode = false;
    bool autoAEFlashMode =  false;
    switch (mAEMode) {
        case ANDROID_CONTROL_AE_MODE_ON_AUTO_FLASH:
        case ANDROID_CONTROL_AE_MODE_ON_ALWAYS_FLASH:
        case ANDROID_CONTROL_AE_MODE_ON_AUTO_FLASH_REDEYE:
            autoAEFlashMode =  true;
            [[fallthrough]];
        case ANDROID_CONTROL_AE_MODE_ON:
            autoAEMode = true;
    };
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
                // Use last valid value
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
                // Use last valid value
            }
        }

        if (mSensorFrameDuration < mSensorExposureTime) {
            mSensorFrameDuration = mSensorExposureTime;
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
                // Use last valid value
            }
        }
        mAEState = ANDROID_CONTROL_AE_STATE_INACTIVE;
    } else if (mIsBackwardCompatible && autoAEMode) {
        auto ret = doFakeAE();
        if (ret != OK) {
            ALOGE("%s: Failed fake AE: %d, (%s)", __FUNCTION__, ret,
                    strerror(-ret));
        }

        // Do AE compensation on the results of the AE
        ret = compensateAE();
        if (ret != OK) {
            ALOGE("%s: Failed duiring AE compensation: %d, (%s)", __FUNCTION__, ret,
                    strerror(-ret));
        }
    } else {
        ALOGI("%s: No emulation for AE mode: %d using previous sensor settings!", __FUNCTION__,
                mAEMode);
    }

    if (mIsFlashSupported) {
        mFlashState = ANDROID_FLASH_STATE_READY;
        // Flash fires only if the request manually enables it (SINGLE/TORCH)
        // and the appropriate AE mode is set or during still capture with auto
        // flash AE modes.
        bool manualFlashMode = false;
        auto ret = mRequestSettings->Get(ANDROID_FLASH_MODE, &entry);
        if ((ret == OK) && (entry.count == 1)) {
            if ((entry.data.u8[0] == ANDROID_FLASH_MODE_SINGLE) ||
                    (entry.data.u8[0] == ANDROID_FLASH_MODE_TORCH)) {
                manualFlashMode = true;
            }
        }
        if (manualFlashMode && ((mAEMode == ANDROID_CONTROL_AE_MODE_OFF) ||
                    (mAEMode == ANDROID_CONTROL_AE_MODE_ON))) {
            mFlashState = ANDROID_FLASH_STATE_FIRED;
        } else {
            bool isStillCapture = false;
            ret = mRequestSettings->Get(ANDROID_CONTROL_CAPTURE_INTENT, &entry);
            if ((ret == OK) && (entry.count == 1)) {
                if (entry.data.u8[0] == ANDROID_CONTROL_CAPTURE_INTENT_STILL_CAPTURE) {
                    isStillCapture = true;
                }
            }
            if (isStillCapture && autoAEFlashMode) {
                mFlashState = ANDROID_FLASH_STATE_FIRED;
            }
        }
    } else {
        mFlashState = ANDROID_FLASH_STATE_UNAVAILABLE;
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
                ALOGE("%s: AE mode: %d not supported using last valid mode!", __FUNCTION__,
                        entry.data.u8[0]);
            }
        }

        ret = mRequestSettings->Get(ANDROID_CONTROL_AWB_MODE, &entry);
        if ((ret == OK) && (entry.count == 1)) {
            if (mAvailableAWBModes.find(entry.data.u8[0]) != mAvailableAWBModes.end()) {
                mAWBMode = entry.data.u8[0];
            } else {
                ALOGE("%s: AWB mode: %d not supported using last valid mode!", __FUNCTION__,
                        entry.data.u8[0]);
            }
        }

        ret = mRequestSettings->Get(ANDROID_CONTROL_AF_MODE, &entry);
        if ((ret == OK) && (entry.count == 1)) {
            if (mAvailableAFModes.find(entry.data.u8[0]) != mAvailableAFModes.end()) {
                mAFModeChanged = mAFMode != entry.data.u8[0];
                mAFMode = entry.data.u8[0];
            } else {
                ALOGE("%s: AF mode: %d not supported using last valid mode!", __FUNCTION__,
                        entry.data.u8[0]);
            }
        }
    } else {
        auto it = mSceneOverrides.find(mSceneMode);
        if (it != mSceneOverrides.end()) {
            mAEMode = it->second.aeMode;
            mAWBMode = it->second.awbMode;
            mAFModeChanged = mAFMode != entry.data.u8[0];
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

    ret = processAWB();
    if (ret != OK) {
        return ret;
    }

    ret = processAF();
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
    result->result_metadata = HalCameraMetadata::Clone(mRequestSettings.get());
    result->input_buffers = request.inputBuffers;
    result->partial_result = mPartialResultCount;

    // Results supported on all emulated devices
    result->result_metadata->Set(ANDROID_REQUEST_PIPELINE_DEPTH, &mMaxPipelineDepth, 1);
    result->result_metadata->Set(ANDROID_CONTROL_MODE, &mControlMode, 1);
    result->result_metadata->Set(ANDROID_CONTROL_AF_MODE, &mAFMode, 1);
    result->result_metadata->Set(ANDROID_CONTROL_AF_STATE, &mAFState, 1);
    result->result_metadata->Set(ANDROID_CONTROL_AWB_MODE, &mAWBMode, 1);
    result->result_metadata->Set(ANDROID_CONTROL_AWB_STATE, &mAWBState, 1);
    result->result_metadata->Set(ANDROID_CONTROL_AE_MODE, &mAEMode, 1);
    result->result_metadata->Set(ANDROID_CONTROL_AE_STATE, &mAEState, 1);
    int32_t fpsRange[] = {mAETargetFPS.minFPS, mAETargetFPS.maxFPS};
    result->result_metadata->Set(ANDROID_CONTROL_AE_TARGET_FPS_RANGE, fpsRange,
            ARRAY_SIZE(fpsRange));
    result->result_metadata->Set(ANDROID_FLASH_STATE, &mFlashState, 1);
    result->result_metadata->Set(ANDROID_LENS_STATE, &mLensState, 1);

    // Results depending on device capability and features
    if (mIsBackwardCompatible) {
        result->result_metadata->Set(ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER, &mAETrigger, 1);
        result->result_metadata->Set(ANDROID_CONTROL_AF_TRIGGER, &mAFTrigger, 1);
        uint8_t vstabMode = ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_OFF;
        result->result_metadata->Set(ANDROID_CONTROL_VIDEO_STABILIZATION_MODE, &vstabMode, 1);
        if (mExposureCompensationSupported) {
            result->result_metadata->Set(ANDROID_CONTROL_AE_EXPOSURE_COMPENSATION,
                    &mExposureCompensation, 1);
        }
    }
    if (mAELockAvailable && mReportAELock) {
        result->result_metadata->Set(ANDROID_CONTROL_AE_LOCK, &mAELock, 1);
    }
    if (mAWBLockAvailable && mReportAWBLock) {
        result->result_metadata->Set(ANDROID_CONTROL_AWB_LOCK, &mAWBLock, 1);
    }
    if (mScenesSupported) {
        result->result_metadata->Set(ANDROID_CONTROL_SCENE_MODE, &mSceneMode, 1);
    }
    if (mMaxAERegions > 0) {
        result->result_metadata->Set(ANDROID_CONTROL_AE_REGIONS, mAEMeteringRegion,
                ARRAY_SIZE(mAEMeteringRegion));
    }
    if (mMaxAWBRegions > 0) {
        result->result_metadata->Set(ANDROID_CONTROL_AWB_REGIONS, mAWBMeteringRegion,
                ARRAY_SIZE(mAWBMeteringRegion));
    }
    if (mMaxAFRegions > 0) {
        result->result_metadata->Set(ANDROID_CONTROL_AF_REGIONS, mAFMeteringRegion,
                ARRAY_SIZE(mAFMeteringRegion));
    }
    if (mReportExposureTime) {
        result->result_metadata->Set(ANDROID_SENSOR_EXPOSURE_TIME, &mSensorExposureTime, 1);
    }
    if (mReportFrameDuration) {
        result->result_metadata->Set(ANDROID_SENSOR_FRAME_DURATION, &mSensorFrameDuration, 1);
    }
    if (mReportSensitivity) {
        result->result_metadata->Set(ANDROID_SENSOR_SENSITIVITY, &mSensorSensitivity, 1);
    }
    if (mReportRollingShutterSkew) {
        result->result_metadata->Set(ANDROID_SENSOR_ROLLING_SHUTTER_SKEW,
                &EmulatedSensor::kSupportedFrameDurationRange[0], 1);
    }
    if (mReportPostRawBoost) {
        result->result_metadata->Set(ANDROID_CONTROL_POST_RAW_SENSITIVITY_BOOST, &mPostRawBoost, 1);
    }
    if (mReportFocusDistance) {
        result->result_metadata->Set(ANDROID_LENS_FOCUS_DISTANCE, &mFocusDistance, 1);
    }
    if (mReportFocusRange) {
        float focusRange [2] = {0.f};
        if (mMinimumFocusDistance > .0f) {
            focusRange[0] = 1 / mMinimumFocusDistance;
        }
        result->result_metadata->Set(ANDROID_LENS_FOCUS_RANGE, focusRange, ARRAY_SIZE(focusRange));
    }
    if (mReportFilterDensity) {
        result->result_metadata->Set(ANDROID_LENS_FILTER_DENSITY, &mFilterDensity, 1);
    }
    if (mReportOISMode) {
        result->result_metadata->Set(ANDROID_LENS_OPTICAL_STABILIZATION_MODE, &mOISMode, 1);
    }
    if (mReportPoseRotation) {
        result->result_metadata->Set(ANDROID_LENS_POSE_ROTATION, mPoseRotation,
                ARRAY_SIZE(mPoseRotation));
    }
    if (mReportPoseTranslation) {
        result->result_metadata->Set(ANDROID_LENS_POSE_TRANSLATION, mPoseTranslation,
                ARRAY_SIZE(mPoseTranslation));
    }
    if (mReportIntrinsicCalibration) {
        result->result_metadata->Set(ANDROID_LENS_INTRINSIC_CALIBRATION, mIntrinsicCalibration,
                ARRAY_SIZE(mIntrinsicCalibration));
    }
    if (mReportDistortion) {
        result->result_metadata->Set(ANDROID_LENS_DISTORTION, mDistortion,
                ARRAY_SIZE(mDistortion));
    }
    if (mReportBlackLevelLock) {
        result->result_metadata->Set(ANDROID_BLACK_LEVEL_LOCK, &mBlackLevelLock, 1);
    }
    if (mReportSceneFlicker) {
        result->result_metadata->Set(ANDROID_STATISTICS_SCENE_FLICKER, &mCurrentSceneFlicker, 1);
    }

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

    mReportRollingShutterSkew = mAvailableResults.find(ANDROID_SENSOR_ROLLING_SHUTTER_SKEW) !=
            mAvailableResults.end();
    mReportSensitivity = mAvailableResults.find(ANDROID_SENSOR_SENSITIVITY) !=
        mAvailableRequests.end();
    mReportExposureTime = mAvailableResults.find(ANDROID_SENSOR_EXPOSURE_TIME) !=
        mAvailableRequests.end();
    mReportFrameDuration = mAvailableResults.find(ANDROID_SENSOR_FRAME_DURATION) !=
        mAvailableRequests.end();

    if (mAvailableResults.find(ANDROID_SENSOR_TIMESTAMP) == mAvailableRequests.end()) {
        ALOGE("%s: Sensor timestamp must always be part of the results!", __FUNCTION__);
        return BAD_VALUE;
    }

    ret = mStaticMetadata->Get(ANDROID_SENSOR_AVAILABLE_TEST_PATTERN_MODES, &entry);
    if (ret == OK) {
        mAvailableTestPatternModes.insert(entry.data.i32, entry.data.i32 + entry.count);
    } else {
        ALOGE("%s: No available test pattern modes!", __FUNCTION__);
        return BAD_VALUE;
    }

    mSensorExposureTime = getClosestValue(EmulatedSensor::kDefaultExposureTime,
            mSensorExposureTimeRange.first, mSensorExposureTimeRange.second);
    mSensorFrameDuration = getClosestValue(EmulatedSensor::kDefaultFrameDuration,
            EmulatedSensor::kSupportedFrameDurationRange[0], mSensorMaxFrameDuration);
    mSensorSensitivity = getClosestValue(EmulatedSensor::kDefaultSensitivity,
            mSensorSensitivityRange.first, mSensorSensitivityRange.second);

    bool offTestPatternModeSupported = mAvailableTestPatternModes.find(
            ANDROID_SENSOR_TEST_PATTERN_MODE_OFF) != mAvailableTestPatternModes.end();
    int32_t testPatternMode = (offTestPatternModeSupported) ? ANDROID_SENSOR_TEST_PATTERN_MODE_OFF :
            *mAvailableTestPatternModes.begin();
    for (size_t idx = 0; idx < kTemplateCount; idx++) {
        if (mDefaultRequests[idx].get() == nullptr) {
            continue;
        }

        mDefaultRequests[idx]->Set(ANDROID_SENSOR_EXPOSURE_TIME, &mSensorExposureTime, 1);
        mDefaultRequests[idx]->Set(ANDROID_SENSOR_FRAME_DURATION, &mSensorFrameDuration, 1);
        mDefaultRequests[idx]->Set(ANDROID_SENSOR_SENSITIVITY, &mSensorSensitivity, 1);
        mDefaultRequests[idx]->Set(ANDROID_SENSOR_TEST_PATTERN_MODE, &testPatternMode, 1);
    }

    return OK;
}

status_t EmulatedRequestState::initializeStatisticsDefaults() {
    camera_metadata_ro_entry_t entry;
    auto ret = mStaticMetadata->Get(ANDROID_STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES, &entry);
    if (ret == OK) {
        mAvailableFaceDetectModes.insert(entry.data.u8, entry.data.u8 + entry.count);
    } else {
        ALOGE("%s: No available face detect modes!", __FUNCTION__);
        return BAD_VALUE;
    }

    ret = mStaticMetadata->Get(ANDROID_STATISTICS_INFO_AVAILABLE_LENS_SHADING_MAP_MODES, &entry);
    if (ret == OK) {
        mAvailableLensShadingMapModes.insert(entry.data.u8, entry.data.u8 + entry.count);
    } else {
        ALOGE("%s: No available lens shading modes!", __FUNCTION__);
        return BAD_VALUE;
    }

    ret = mStaticMetadata->Get(ANDROID_STATISTICS_INFO_AVAILABLE_HOT_PIXEL_MAP_MODES, &entry);
    if (ret == OK) {
        mAvailableHotPixelMapModes.insert(entry.data.u8, entry.data.u8 + entry.count);
    } else if (mIsRAWCapable) {
        ALOGE("%s: RAW capable device must support hot pixel map modes!", __FUNCTION__);
        return BAD_VALUE;
    } else {
        mAvailableHotPixelMapModes.emplace(ANDROID_STATISTICS_HOT_PIXEL_MAP_MODE_OFF);
    }

    mReportSceneFlicker = mAvailableResults.find(ANDROID_STATISTICS_SCENE_FLICKER) !=
        mAvailableResults.end();

    uint8_t faceDetectMode = *mAvailableFaceDetectModes.begin();
    uint8_t hotPixelMapMode = *mAvailableHotPixelMapModes.begin();
    uint8_t lensShadingMapMode = *mAvailableLensShadingMapModes.begin();
    for (size_t idx = 0; idx < kTemplateCount; idx++) {
        if (mDefaultRequests[idx].get() == nullptr) {
            continue;
        }

        mDefaultRequests[idx]->Set(ANDROID_STATISTICS_FACE_DETECT_MODE, &faceDetectMode, 1);
        mDefaultRequests[idx]->Set(ANDROID_STATISTICS_HOT_PIXEL_MAP_MODE, &hotPixelMapMode, 1);
        mDefaultRequests[idx]->Set(ANDROID_STATISTICS_LENS_SHADING_MAP_MODE, &lensShadingMapMode,
                1);
    }

    return initializeBlackLevelDefaults();
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

    if (mAvailableRequests.find(ANDROID_CONTROL_AF_MODE) == mAvailableRequests.end()) {
        ALOGE("%s: Clients must be able to set AF mode!", __FUNCTION__);
        return BAD_VALUE;
    }

    if (mAvailableRequests.find(ANDROID_CONTROL_AF_TRIGGER) == mAvailableRequests.end()) {
        ALOGE("%s: Clients must be able to set AF trigger!", __FUNCTION__);
        return BAD_VALUE;
    }
    if (mAvailableResults.find(ANDROID_CONTROL_AF_TRIGGER) == mAvailableResults.end()) {
        ALOGE("%s: AF trigger must be reported!", __FUNCTION__);
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

    bool autoModePresent = mAvailableAFModes.find(ANDROID_CONTROL_AF_MODE_AUTO) !=
            mAvailableAFModes.end();
    bool pictureCAFModePresent = mAvailableAFModes.find(
            ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE) != mAvailableAFModes.end();
    bool videoCAFModePresent = mAvailableAFModes.find(
            ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO) != mAvailableAFModes.end();
    mAFSupported = autoModePresent && (mMinimumFocusDistance > .0f);
    mPictureCAFSupported = pictureCAFModePresent && (mMinimumFocusDistance > .0f);
    mVideoCAFSupported = videoCAFModePresent && (mMinimumFocusDistance > .0f);

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

    ret = mStaticMetadata->Get(ANDROID_CONTROL_AWB_LOCK_AVAILABLE, &entry);
    if ((ret == OK) && (entry.count == 1)) {
        mAWBLockAvailable = entry.data.u8[0] == ANDROID_CONTROL_AWB_LOCK_AVAILABLE_TRUE;
    } else {
        ALOGV("%s: No available AWB lock!", __FUNCTION__);
        mAWBLockAvailable = false;
    }
    mReportAWBLock = mAvailableResults.find(ANDROID_CONTROL_AWB_LOCK) != mAvailableResults.end();

    return OK;
}

status_t EmulatedRequestState::initializeBlackLevelDefaults() {
    if (mSupportedHWLevel >= ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_FULL) {
        if (mAvailableRequests.find(ANDROID_BLACK_LEVEL_LOCK) == mAvailableRequests.end()) {
            ALOGE("%s: Full or above capable devices must be able to set the black level lock!",
                    __FUNCTION__);
            return BAD_VALUE;
        }

        if (mAvailableResults.find(ANDROID_BLACK_LEVEL_LOCK) == mAvailableResults.end()) {
            ALOGE("%s: Full or above capable devices must be able to report the black level lock!",
                    __FUNCTION__);
            return BAD_VALUE;
        }

        mReportBlackLevelLock = true;
        uint8_t blackLevelLock = ANDROID_BLACK_LEVEL_LOCK_OFF;
        for (size_t idx = 0; idx < kTemplateCount; idx++) {
            if (mDefaultRequests[idx].get() == nullptr) {
                continue;
            }

            mDefaultRequests[idx]->Set(ANDROID_BLACK_LEVEL_LOCK, &blackLevelLock, 1);
        }
    }

    return initializeEdgeDefaults();
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

    if (mAvailableResults.find(ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER) ==
            mAvailableResults.end()) {
        ALOGE("%s: AE pre-capture trigger must be reported!", __FUNCTION__);
        return BAD_VALUE;
    }

    ret = mStaticMetadata->Get(ANDROID_CONTROL_AE_AVAILABLE_ANTIBANDING_MODES, &entry);
    if (ret == OK) {
        mAvailableAntibandingModes.insert(entry.data.u8, entry.data.u8 + entry.count);
    } else {
        ALOGE("%s: No available antibanding modes!", __FUNCTION__);
        return BAD_VALUE;
    }

    ret = mStaticMetadata->Get(ANDROID_CONTROL_AE_COMPENSATION_RANGE, &entry);
    if ((ret == OK) && (entry.count == 2)) {
        mExposureCompensationRange[0] = entry.data.i32[0];
        mExposureCompensationRange[1] = entry.data.i32[1];
    } else {
        ALOGE("%s: No available exposure compensation range!", __FUNCTION__);
        return BAD_VALUE;
    }

    ret = mStaticMetadata->Get(ANDROID_CONTROL_AE_COMPENSATION_STEP, &entry);
    if ((ret == OK) && (entry.count == 1)) {
        mExposureCompensationStep = entry.data.r[0];
    } else {
        ALOGE("%s: No available exposure compensation step!", __FUNCTION__);
        return BAD_VALUE;
    }

    bool aeCompRequests = mAvailableRequests.find(ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER) !=
        mAvailableRequests.end();
    bool aeCompResults = mAvailableResults.find(ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER) !=
        mAvailableResults.end();
    mExposureCompensationSupported = ((mExposureCompensationRange[0] < 0) &&
            (mExposureCompensationRange[1] > 0) &&
            (mExposureCompensationStep.denominator > 0) &&
            (mExposureCompensationStep.numerator > 0)) && aeCompResults && aeCompRequests;

    return OK;
}

status_t EmulatedRequestState::initializeMeteringRegionDefault(uint32_t tag,
        int32_t *region /*out*/) {
    if (region == nullptr) {
        return BAD_VALUE;
    }
    if (mAvailableRequests.find(tag) == mAvailableRequests.end()) {
        ALOGE("%s: %d metering region configuration must be supported!", __FUNCTION__, tag);
        return BAD_VALUE;
    }
    if (mAvailableResults.find(tag) == mAvailableResults.end()) {
        ALOGE("%s: %d metering region must be reported!", __FUNCTION__, tag);
        return BAD_VALUE;
    }

    region[0] = mScalerCropRegionDefault[0];
    region[1] = mScalerCropRegionDefault[1];
    region[2] = mScalerCropRegionDefault[2];
    region[3] = mScalerCropRegionDefault[3];
    region[4] = 0;

    return OK;
}

status_t EmulatedRequestState::initializeControlDefaults() {
    camera_metadata_ro_entry_t entry;
    int32_t meteringArea[5] = {0}; // (top, left, width, height, wight)
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

    ret = mStaticMetadata->Get(ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, &entry);
    if ((ret == OK) && ((entry.count % 2) == 0)) {
        mAvailableFPSRanges.reserve(entry.count / 2);
        for (size_t i = 0; i < entry.count; i += 2) {
            FPSRange range(entry.data.i32[i], entry.data.i32[i+1]);
            if (range.minFPS > range.maxFPS) {
                ALOGE("%s: Mininum framerate: %d bigger than maximum framerate: %d", __FUNCTION__,
                        range.minFPS, range.maxFPS);
                return BAD_VALUE;
            }
            if ((range.maxFPS >= kMinimumStreamingFPS) && (range.maxFPS == range.minFPS) &&
                    (mAETargetFPS.maxFPS == 0)) {
                mAETargetFPS = range;
            }
            mAvailableFPSRanges.push_back(range);
        }
    } else {
        ALOGE("%s: No available framerate ranges!", __FUNCTION__);
        return BAD_VALUE;
    }

    if (mAETargetFPS.maxFPS == 0) {
        ALOGE("%s: No minimum streaming capable framerate range available!", __FUNCTION__);
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

    if (mIsBackwardCompatible) {
        ret = mStaticMetadata->Get(ANDROID_CONTROL_POST_RAW_SENSITIVITY_BOOST, &entry);
        if (ret == OK) {
            mPostRawBoost = entry.data.i32[0];
        } else {
            ALOGW("%s: No available post RAW boost! Setting default!", __FUNCTION__);
            mPostRawBoost = 100;
        }
        mReportPostRawBoost = mAvailableResults.find(ANDROID_CONTROL_POST_RAW_SENSITIVITY_BOOST) !=
            mAvailableResults.end();

        ret = mStaticMetadata->Get(ANDROID_CONTROL_AVAILABLE_EFFECTS, &entry);
        if ((ret == OK) && (entry.count > 0)) {
            mAvailableEffects.insert(entry.data.u8, entry.data.u8 + entry.count);
            if (mAvailableEffects.find(ANDROID_CONTROL_EFFECT_MODE_OFF) ==
                    mAvailableEffects.end()) {
                ALOGE("%s: Off color effect mode not supported!", __FUNCTION__);
                return BAD_VALUE;
            }
        } else {
            ALOGE("%s: No available effects!", __FUNCTION__);
            return BAD_VALUE;
        }

        ret = mStaticMetadata->Get(ANDROID_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES, &entry);
        if ((ret == OK) && (entry.count > 0)) {
            mAvailableVSTABModes.insert(entry.data.u8, entry.data.u8 + entry.count);
            if (mAvailableVSTABModes.find(ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_OFF) ==
                    mAvailableVSTABModes.end()) {
                ALOGE("%s: Off video stabilization mode not supported!", __FUNCTION__);
                return BAD_VALUE;
            }
        } else {
            ALOGE("%s: No available video stabilization modes!", __FUNCTION__);
            return BAD_VALUE;
        }

        ret = mStaticMetadata->Get(ANDROID_CONTROL_MAX_REGIONS, &entry);
        if ((ret == OK) && (entry.count == 3)) {
            mMaxAERegions = entry.data.i32[0];
            mMaxAWBRegions = entry.data.i32[1];
            mMaxAFRegions = entry.data.i32[2];
        } else {
            ALOGE("%s: Metering regions must be available for backward compatible devices!",
                    __FUNCTION__);
            return BAD_VALUE;
        }

        if ((mSupportedHWLevel >= ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_FULL) &&
                ((mMaxAERegions == 0) || (mMaxAFRegions == 0))) {
            ALOGE("%s: Full and higher level cameras must support at AF and AE metering regions",
                    __FUNCTION__);
            return BAD_VALUE;
        }

        if (mMaxAERegions > 0) {
            ret = initializeMeteringRegionDefault(ANDROID_CONTROL_AE_REGIONS, mAEMeteringRegion);
            if (ret != OK) {
                return ret;
            }
        }

        if (mMaxAWBRegions > 0) {
            ret = initializeMeteringRegionDefault(ANDROID_CONTROL_AWB_REGIONS, mAWBMeteringRegion);
            if (ret != OK) {
                return ret;
            }
        }

        if (mMaxAFRegions > 0) {
            ret = initializeMeteringRegionDefault(ANDROID_CONTROL_AF_REGIONS, mAFMeteringRegion);
            if (ret != OK) {
                return ret;
            }
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
        afMode = mAFSupported ? ANDROID_CONTROL_AF_MODE_AUTO : ANDROID_CONTROL_AF_MODE_OFF;
        sceneMode = ANDROID_CONTROL_SCENE_MODE_DISABLED;
        uint8_t effectMode = ANDROID_CONTROL_EFFECT_MODE_OFF;
        uint8_t aeLock = ANDROID_CONTROL_AE_LOCK_OFF;
        uint8_t awbLock = ANDROID_CONTROL_AWB_LOCK_OFF;
        int32_t aeTargetFPS [] = {mAETargetFPS.minFPS, mAETargetFPS.maxFPS};
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
                if (mPictureCAFSupported) {
                    afMode = ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE;
                }
                break;
            case RequestTemplate::kStillCapture:
                intent = ANDROID_CONTROL_CAPTURE_INTENT_STILL_CAPTURE;
                if (mPictureCAFSupported) {
                    afMode = ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE;
                }
                break;
            case RequestTemplate::kVideoRecord:
                intent = ANDROID_CONTROL_CAPTURE_INTENT_VIDEO_RECORD;
                if (mVideoCAFSupported) {
                    afMode = ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO;
                }
                break;
            case RequestTemplate::kVideoSnapshot:
                intent = ANDROID_CONTROL_CAPTURE_INTENT_VIDEO_SNAPSHOT;
                if (mVideoCAFSupported) {
                    afMode = ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO;
                }
                break;
            default:
                //Noop
                break;
        }

        if (intent != ANDROID_CONTROL_CAPTURE_INTENT_CUSTOM) {
            mDefaultRequests[idx]->Set(ANDROID_CONTROL_CAPTURE_INTENT, &intent, 1);
            mDefaultRequests[idx]->Set(ANDROID_CONTROL_MODE, &controlMode, 1);
            mDefaultRequests[idx]->Set(ANDROID_CONTROL_AE_MODE, &aeMode, 1);
            mDefaultRequests[idx]->Set(ANDROID_CONTROL_AE_TARGET_FPS_RANGE, aeTargetFPS,
                        ARRAY_SIZE(aeTargetFPS));
            mDefaultRequests[idx]->Set(ANDROID_CONTROL_AWB_MODE, &awbMode, 1);
            mDefaultRequests[idx]->Set(ANDROID_CONTROL_AF_MODE, &afMode, 1);
            if (mIsBackwardCompatible) {
                mDefaultRequests[idx]->Set(ANDROID_CONTROL_POST_RAW_SENSITIVITY_BOOST, &mPostRawBoost,
                        1);
                if (mAELockAvailable) {
                    mDefaultRequests[idx]->Set(ANDROID_CONTROL_AE_LOCK, &aeLock, 1);
                }
                if (mAWBLockAvailable) {
                    mDefaultRequests[idx]->Set(ANDROID_CONTROL_AWB_LOCK, &awbLock, 1);
                }
                if (mScenesSupported) {
                    mDefaultRequests[idx]->Set(ANDROID_CONTROL_SCENE_MODE, &sceneMode, 1);
                }
                if (mMaxAERegions > 0) {
                    mDefaultRequests[idx]->Set(ANDROID_CONTROL_AE_REGIONS, meteringArea,
                            ARRAY_SIZE(meteringArea));
                }
                if (mMaxAWBRegions > 0) {
                    mDefaultRequests[idx]->Set(ANDROID_CONTROL_AWB_REGIONS, meteringArea,
                            ARRAY_SIZE(meteringArea));
                }
                if (mMaxAFRegions > 0) {
                    mDefaultRequests[idx]->Set(ANDROID_CONTROL_AF_REGIONS, meteringArea,
                            ARRAY_SIZE(meteringArea));
                }
                if (mExposureCompensationSupported) {
                    mDefaultRequests[idx]->Set(ANDROID_CONTROL_AE_EXPOSURE_COMPENSATION,
                            &mExposureCompensation, 1);
                }
                bool isAutoAntbandingSupported = mAvailableAntibandingModes.find(
                        ANDROID_CONTROL_AE_ANTIBANDING_MODE_AUTO) !=
                        mAvailableAntibandingModes.end();
                uint8_t antibandingMode = isAutoAntbandingSupported ?
                        ANDROID_CONTROL_AE_ANTIBANDING_MODE_AUTO :
                        *mAvailableAntibandingModes.begin();
                mDefaultRequests[idx]->Set(ANDROID_CONTROL_AE_ANTIBANDING_MODE, &antibandingMode,
                        1);
                mDefaultRequests[idx]->Set(ANDROID_CONTROL_EFFECT_MODE, &effectMode, 1);
                uint8_t aeTrigger = ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_IDLE;
                mDefaultRequests[idx]->Set(ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER, &aeTrigger, 1);
                uint8_t afTrigger = ANDROID_CONTROL_AF_TRIGGER_IDLE;
                mDefaultRequests[idx]->Set(ANDROID_CONTROL_AF_TRIGGER, &afTrigger, 1);
            }
        }
    }

    return initializeHotPixelDefaults();
}

status_t EmulatedRequestState::initializeTonemapDefaults() {
    if (mIsBackwardCompatible) {
        camera_metadata_ro_entry_t entry;
        auto ret = mStaticMetadata->Get(ANDROID_TONEMAP_AVAILABLE_TONE_MAP_MODES, &entry);
        if (ret == OK) {
            mAvailableTonemapModes.insert(entry.data.u8, entry.data.u8 + entry.count);
        } else {
            ALOGE("%s: No available tonemap modes!", __FUNCTION__);
            return BAD_VALUE;
        }

        if ((mSupportedHWLevel >= ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_FULL) &&
                (mAvailableTonemapModes.size() < 3)){
            ALOGE("%s: Full and higher level cameras must support at least three or more tonemap modes",
                    __FUNCTION__);
            return BAD_VALUE;
        }

        bool fastModeSupported = mAvailableTonemapModes.find(ANDROID_TONEMAP_MODE_FAST) !=
            mAvailableTonemapModes.end();
        bool hqModeSupported = mAvailableTonemapModes.find(ANDROID_TONEMAP_MODE_HIGH_QUALITY) !=
            mAvailableTonemapModes.end();
        uint8_t tonemapMode = *mAvailableTonemapModes.begin();
        for (size_t idx = 0; idx < kTemplateCount; idx++) {
            if (mDefaultRequests[idx].get() == nullptr) {
                continue;
            }

            switch(static_cast<RequestTemplate>(idx)) {
                case RequestTemplate::kVideoRecord: // Pass-through
                case RequestTemplate::kPreview:
                    if (fastModeSupported) {
                        tonemapMode = ANDROID_TONEMAP_MODE_FAST;
                    }
                    break;
                case RequestTemplate::kVideoSnapshot: // Pass-through
                case RequestTemplate::kStillCapture:
                    if (hqModeSupported) {
                        tonemapMode = ANDROID_TONEMAP_MODE_HIGH_QUALITY;
                    }
                    break;
                default:
                    //Noop
                    break;
            }

            mDefaultRequests[idx]->Set(ANDROID_TONEMAP_MODE, &tonemapMode, 1);
            mDefaultRequests[idx]->Set(ANDROID_TONEMAP_CURVE_RED,
                    EmulatedSensor::kDefaultToneMapCurveRed,
                    ARRAY_SIZE(EmulatedSensor::kDefaultToneMapCurveRed));
            mDefaultRequests[idx]->Set(ANDROID_TONEMAP_CURVE_GREEN,
                    EmulatedSensor::kDefaultToneMapCurveGreen,
                    ARRAY_SIZE(EmulatedSensor::kDefaultToneMapCurveGreen));
            mDefaultRequests[idx]->Set(ANDROID_TONEMAP_CURVE_BLUE,
                    EmulatedSensor::kDefaultToneMapCurveBlue,
                    ARRAY_SIZE(EmulatedSensor::kDefaultToneMapCurveBlue));
        }
    }

    return initializeStatisticsDefaults();
}

status_t EmulatedRequestState::initializeEdgeDefaults() {
    if (mIsBackwardCompatible) {
        camera_metadata_ro_entry_t entry;
        auto ret = mStaticMetadata->Get(ANDROID_EDGE_AVAILABLE_EDGE_MODES, &entry);
        if (ret == OK) {
            mAvailableEdgeModes.insert(entry.data.u8, entry.data.u8 + entry.count);
        } else {
            ALOGE("%s: No available edge modes!", __FUNCTION__);
            return BAD_VALUE;
        }

        bool isFastModeSupported = mAvailableEdgeModes.find(ANDROID_EDGE_MODE_FAST) !=
            mAvailableEdgeModes.end();
        bool isHQModeSupported = mAvailableEdgeModes.find(ANDROID_EDGE_MODE_HIGH_QUALITY) !=
            mAvailableEdgeModes.end();
        uint8_t edgeMode = *mAvailableAEModes.begin();
        for (size_t idx = 0; idx < kTemplateCount; idx++) {
            if (mDefaultRequests[idx].get() == nullptr) {
                continue;
            }

            switch(static_cast<RequestTemplate>(idx)) {
                case RequestTemplate::kVideoRecord: // Pass-through
                case RequestTemplate::kPreview:
                    if (isFastModeSupported) {
                        edgeMode = ANDROID_EDGE_MODE_FAST;
                    }
                    break;
                case RequestTemplate::kVideoSnapshot: // Pass-through
                case RequestTemplate::kStillCapture:
                    if (isHQModeSupported) {
                        edgeMode = ANDROID_EDGE_MODE_HIGH_QUALITY;
                    }
                    break;
                default:
                    //Noop
                    break;
            }

            mDefaultRequests[idx]->Set(ANDROID_EDGE_MODE, &edgeMode, 1);
        }
    }

    return initializeShadingDefaults();
}

status_t EmulatedRequestState::initializeColorCorrectionDefaults() {
    camera_metadata_ro_entry_t entry;
    auto ret = mStaticMetadata->Get(ANDROID_COLOR_CORRECTION_AVAILABLE_ABERRATION_MODES,
            &entry);
    if (ret == OK) {
        mAvailableColorAberrationModes.insert(entry.data.u8, entry.data.u8 + entry.count);
    } else if (mSupportsManualPostProcessing) {
        ALOGE("%s: Devices capable of manual post-processing must support color abberation!",
                __FUNCTION__);
        return BAD_VALUE;
    }

    if (!mAvailableColorAberrationModes.empty()) {
        bool isFastModeSupported = mAvailableColorAberrationModes.find(
                ANDROID_COLOR_CORRECTION_ABERRATION_MODE_FAST) !=
            mAvailableColorAberrationModes.end();
        bool isHQModeSupported = mAvailableColorAberrationModes.find(
                ANDROID_COLOR_CORRECTION_ABERRATION_MODE_HIGH_QUALITY) !=
            mAvailableColorAberrationModes.end();
        uint8_t colorAberration = *mAvailableColorAberrationModes.begin();
        uint8_t colorCorrectionMode = ANDROID_COLOR_CORRECTION_MODE_FAST;
        for (size_t idx = 0; idx < kTemplateCount; idx++) {
            if (mDefaultRequests[idx].get() == nullptr) {
                continue;
            }

            switch(static_cast<RequestTemplate>(idx)) {
                case RequestTemplate::kVideoRecord: // Pass-through
                case RequestTemplate::kPreview:
                    if (isFastModeSupported) {
                        colorAberration = ANDROID_COLOR_CORRECTION_ABERRATION_MODE_FAST;
                    }
                    break;
                case RequestTemplate::kVideoSnapshot: // Pass-through
                case RequestTemplate::kStillCapture:
                    if (isHQModeSupported) {
                        colorAberration = ANDROID_COLOR_CORRECTION_ABERRATION_MODE_HIGH_QUALITY;
                    }
                    break;
                default:
                    //Noop
                    break;
            }

            mDefaultRequests[idx]->Set(ANDROID_COLOR_CORRECTION_ABERRATION_MODE,
                    &colorAberration, 1);
            if (mIsBackwardCompatible) {
                mDefaultRequests[idx]->Set(ANDROID_COLOR_CORRECTION_MODE, &colorCorrectionMode, 1);
                mDefaultRequests[idx]->Set(ANDROID_COLOR_CORRECTION_TRANSFORM,
                        EmulatedSensor::kDefaultColorTransform,
                        ARRAY_SIZE(EmulatedSensor::kDefaultColorTransform));
                mDefaultRequests[idx]->Set(ANDROID_COLOR_CORRECTION_GAINS,
                        EmulatedSensor::kDefaultColorCorrectionGains,
                        ARRAY_SIZE(EmulatedSensor::kDefaultColorCorrectionGains));
            }
        }
    }

    return initializeSensorDefaults();
}

status_t EmulatedRequestState::initializeScalerDefaults() {
    if (mIsBackwardCompatible) {
        camera_metadata_ro_entry_t entry;
        auto ret = mStaticMetadata->Get(ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE, &entry);
        if ((ret == OK) && (entry.count == 4)) {
            mScalerCropRegionDefault[0] = entry.data.i32[0];
            mScalerCropRegionDefault[1] = entry.data.i32[1];
            mScalerCropRegionDefault[2] = entry.data.i32[2];
            mScalerCropRegionDefault[3] = entry.data.i32[3];
        } else {
            ALOGE("%s: Sensor pixel array size is not available!", __FUNCTION__);
            return BAD_VALUE;
        }

        if (mAvailableRequests.find(ANDROID_SCALER_CROP_REGION) == mAvailableRequests.end()) {
            ALOGE("%s: Backward compatible devices must support scaler crop configuration!",
                    __FUNCTION__);
            return BAD_VALUE;
        }
        if (mAvailableResults.find(ANDROID_SCALER_CROP_REGION) == mAvailableResults.end()) {
            ALOGE("%s: Scaler crop must reported on backward compatible devices!", __FUNCTION__);
            return BAD_VALUE;
        }

        for (size_t idx = 0; idx < kTemplateCount; idx++) {
            if (mDefaultRequests[idx].get() == nullptr) {
                continue;
            }

            mDefaultRequests[idx]->Set(ANDROID_SCALER_CROP_REGION, mScalerCropRegionDefault,
                    ARRAY_SIZE(mScalerCropRegionDefault));
        }
    }

    return initializeControlDefaults();
}

status_t EmulatedRequestState::initializeShadingDefaults() {
    camera_metadata_ro_entry_t entry;
    auto ret = mStaticMetadata->Get(ANDROID_SHADING_AVAILABLE_MODES, &entry);
    if (ret == OK) {
        mAvailableShadingModes.insert(entry.data.u8, entry.data.u8 + entry.count);
    } else {
        ALOGE("%s: No available lens shading modes!", __FUNCTION__);
        return BAD_VALUE;
    }

    if (mSupportsManualPostProcessing && (mAvailableShadingModes.size() < 2)) {
        ALOGE("%s: Devices capable of manual post-processing need to support aleast two"
                " lens shading modes!", __FUNCTION__);
        return BAD_VALUE;
    }

    bool isFastModeSupported = mAvailableShadingModes.find(ANDROID_SHADING_MODE_FAST) !=
            mAvailableShadingModes.end();
    bool isHQModeSupported = mAvailableShadingModes.find(ANDROID_SHADING_MODE_HIGH_QUALITY) !=
            mAvailableShadingModes.end();
    uint8_t shadingMode = *mAvailableShadingModes.begin();
    for (size_t idx = 0; idx < kTemplateCount; idx++) {
        if (mDefaultRequests[idx].get() == nullptr) {
            continue;
        }

        switch(static_cast<RequestTemplate>(idx)) {
            case RequestTemplate::kVideoRecord: // Pass-through
            case RequestTemplate::kPreview:
                if (isFastModeSupported) {
                    shadingMode = ANDROID_SHADING_MODE_FAST;
                }
                break;
            case RequestTemplate::kVideoSnapshot: // Pass-through
            case RequestTemplate::kStillCapture:
                if (isHQModeSupported) {
                    shadingMode = ANDROID_SHADING_MODE_HIGH_QUALITY;
                }
                break;
            default:
                //Noop
                break;
        }

        mDefaultRequests[idx]->Set(ANDROID_SHADING_MODE, &shadingMode, 1);
    }

    return initializeNoiseReductionDefaults();
}

status_t EmulatedRequestState::initializeNoiseReductionDefaults() {
    camera_metadata_ro_entry_t entry;
    auto ret = mStaticMetadata->Get(ANDROID_NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES,
            &entry);
    if (ret == OK) {
        mAvailableNoiseReductionModes.insert(entry.data.u8, entry.data.u8 + entry.count);
    } else {
        ALOGE("%s: No available noise reduction modes!", __FUNCTION__);
        return BAD_VALUE;
    }

    if ((mSupportedHWLevel >= ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_FULL) &&
            (mAvailableNoiseReductionModes.size() < 2)) {
        ALOGE("%s: Full and above device must support aleast two noise reduction modes!",
                __FUNCTION__);
        return BAD_VALUE;
    }

    bool isFastModeSupported = mAvailableNoiseReductionModes.find(
            ANDROID_NOISE_REDUCTION_MODE_FAST) != mAvailableNoiseReductionModes.end();
    bool isHQModeSupported = mAvailableNoiseReductionModes.find(
            ANDROID_NOISE_REDUCTION_MODE_HIGH_QUALITY) != mAvailableNoiseReductionModes.end();
    uint8_t noiseReductionMode = *mAvailableLensShadingMapModes.begin();
    for (size_t idx = 0; idx < kTemplateCount; idx++) {
        if (mDefaultRequests[idx].get() == nullptr) {
            continue;
        }

        switch(static_cast<RequestTemplate>(idx)) {
            case RequestTemplate::kVideoRecord: // Pass-through
            case RequestTemplate::kPreview:
                if (isFastModeSupported) {
                    noiseReductionMode = ANDROID_NOISE_REDUCTION_MODE_FAST;
                }
                break;
            case RequestTemplate::kVideoSnapshot: // Pass-through
            case RequestTemplate::kStillCapture:
                if (isHQModeSupported) {
                    noiseReductionMode = ANDROID_NOISE_REDUCTION_MODE_HIGH_QUALITY;
                }
                break;
            default:
                //Noop
                break;
        }

        mDefaultRequests[idx]->Set(ANDROID_NOISE_REDUCTION_MODE, &noiseReductionMode, 1);
    }

    return initializeColorCorrectionDefaults();
}

status_t EmulatedRequestState::initializeHotPixelDefaults() {
    camera_metadata_ro_entry_t entry;
    auto ret = mStaticMetadata->Get(ANDROID_HOT_PIXEL_AVAILABLE_HOT_PIXEL_MODES, &entry);
    if (ret == OK) {
        mAvailableHotPixelModes.insert(entry.data.u8, entry.data.u8 + entry.count);
    } else {
        ALOGE("%s: No available hotpixel modes!", __FUNCTION__);
        return BAD_VALUE;
    }

    if ((mSupportedHWLevel >= ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_FULL) &&
            (mAvailableHotPixelModes.size() < 2)){
        ALOGE("%s: Full and higher level cameras must support at least fast and hq hotpixel modes",
                __FUNCTION__);
        return BAD_VALUE;
    }

    bool fastModeSupported = mAvailableHotPixelModes.find(ANDROID_HOT_PIXEL_MODE_FAST) !=
        mAvailableHotPixelModes.end();
    bool hqModeSupported = mAvailableHotPixelModes.find(ANDROID_HOT_PIXEL_MODE_HIGH_QUALITY) !=
        mAvailableHotPixelModes.end();
    uint8_t hotpixelMode = *mAvailableHotPixelModes.begin();
    for (size_t idx = 0; idx < kTemplateCount; idx++) {
        if (mDefaultRequests[idx].get() == nullptr) {
            continue;
        }

        switch(static_cast<RequestTemplate>(idx)) {
            case RequestTemplate::kVideoRecord: // Pass-through
            case RequestTemplate::kPreview:
                if (fastModeSupported) {
                    hotpixelMode = ANDROID_HOT_PIXEL_MODE_FAST;
                }
                break;
            case RequestTemplate::kVideoSnapshot: // Pass-through
            case RequestTemplate::kStillCapture:
                if (hqModeSupported) {
                    hotpixelMode = ANDROID_HOT_PIXEL_MODE_HIGH_QUALITY;
                }
                break;
            default:
                //Noop
                break;
        }

        mDefaultRequests[idx]->Set(ANDROID_HOT_PIXEL_MODE, &hotpixelMode, 1);
    }

    return initializeTonemapDefaults();
}

status_t EmulatedRequestState::initializeFlashDefaults() {
    camera_metadata_ro_entry_t entry;
    auto ret = mStaticMetadata->Get(ANDROID_FLASH_INFO_AVAILABLE, &entry);
    if ((ret == OK) && (entry.count == 1)) {
        mIsFlashSupported = entry.data.u8[0];
    } else {
        ALOGE("%s: No available flash info!", __FUNCTION__);
        return BAD_VALUE;
    }

    if (mIsFlashSupported) {
        mFlashState = ANDROID_FLASH_STATE_READY;
    } else {
        mFlashState = ANDROID_FLASH_STATE_UNAVAILABLE;
    }

    uint8_t flashMode = ANDROID_FLASH_MODE_OFF;
    for (size_t idx = 0; idx < kTemplateCount; idx++) {
        if (mDefaultRequests[idx].get() == nullptr) {
            continue;
        }

        mDefaultRequests[idx]->Set(ANDROID_FLASH_MODE, &flashMode, 1);
    }

    return initializeScalerDefaults();
}

status_t EmulatedRequestState::initializeLensDefaults() {
    camera_metadata_ro_entry_t entry;
    auto ret = mStaticMetadata->Get(ANDROID_LENS_INFO_MINIMUM_FOCUS_DISTANCE, &entry);
    if ((ret == OK) && (entry.count == 1)) {
        mMinimumFocusDistance = entry.data.f[0];
    } else {
        ALOGW("%s: No available minimum focus distance assuming fixed focus!", __FUNCTION__);
        mMinimumFocusDistance = .0f;
    }

    ret = mStaticMetadata->Get(ANDROID_LENS_INFO_AVAILABLE_APERTURES, &entry);
    if ((ret == OK) && (entry.count > 0)) {
        // TODO: add support for multiple apertures
        mAperture = entry.data.f[0];
    } else {
        ALOGE("%s: No available aperture!", __FUNCTION__);
        return BAD_VALUE;
    }

    ret = mStaticMetadata->Get(ANDROID_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, &entry);
    if ((ret == OK) && (entry.count > 0)) {
        // TODO: add support for multiple focal lengths
        mFocalLength = entry.data.f[0];
    } else {
        ALOGE("%s: No available focal length!", __FUNCTION__);
        return BAD_VALUE;
    }

    ret = mStaticMetadata->Get(ANDROID_LENS_INFO_AVAILABLE_FILTER_DENSITIES, &entry);
    if ((ret == OK) && (entry.count > 0)) {
        // TODO: add support for multiple filter densities
        mFilterDensity = entry.data.f[0];
    } else {
        ALOGE("%s: No available filter density!", __FUNCTION__);
        return BAD_VALUE;
    }

    ret = mStaticMetadata->Get(ANDROID_LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION, &entry);
    if ((ret == OK) && (entry.count > 0)) {
        // TODO: add support for multiple OIS modes
        mAvailableOISModes.insert(entry.data.u8, entry.data.u8 + entry.count);
        if (mAvailableOISModes.find(ANDROID_LENS_OPTICAL_STABILIZATION_MODE_OFF) ==
                mAvailableOISModes.end()) {
            ALOGE("%s: OIS off mode not supported!", __FUNCTION__);
            return BAD_VALUE;
        }
    } else {
        ALOGE("%s: No available OIS modes!", __FUNCTION__);
        return BAD_VALUE;
    }

    ret = mStaticMetadata->Get(ANDROID_LENS_POSE_ROTATION, &entry);
    if ((ret == OK) && (entry.count == ARRAY_SIZE(mPoseRotation))) {
        memcpy(mPoseRotation, entry.data.f, ARRAY_SIZE(mPoseRotation));
    }
    ret = mStaticMetadata->Get(ANDROID_LENS_POSE_TRANSLATION, &entry);
    if ((ret == OK) && (entry.count == ARRAY_SIZE(mPoseTranslation))) {
        memcpy(mPoseTranslation, entry.data.f, ARRAY_SIZE(mPoseTranslation));
    }
    ret = mStaticMetadata->Get(ANDROID_LENS_INTRINSIC_CALIBRATION, &entry);
    if ((ret == OK) && (entry.count == ARRAY_SIZE(mIntrinsicCalibration))) {
        memcpy(mIntrinsicCalibration, entry.data.f, ARRAY_SIZE(mIntrinsicCalibration));
    }

    ret = mStaticMetadata->Get(ANDROID_LENS_DISTORTION, &entry);
    if ((ret == OK) && (entry.count == ARRAY_SIZE(mDistortion))) {
        memcpy(mDistortion, entry.data.f, ARRAY_SIZE(mDistortion));
    }

    mReportFocusDistance = mAvailableResults.find(ANDROID_LENS_FOCUS_DISTANCE) !=
            mAvailableResults.end();
    mReportFocusRange = mAvailableResults.find(ANDROID_LENS_FOCUS_RANGE) !=
            mAvailableResults.end();
    mReportFilterDensity = mAvailableResults.find(ANDROID_LENS_FILTER_DENSITY) !=
            mAvailableResults.end();
    mReportOISMode = mAvailableResults.find(ANDROID_LENS_OPTICAL_STABILIZATION_MODE) !=
            mAvailableResults.end();
    mReportPoseRotation = mAvailableResults.find(ANDROID_LENS_POSE_ROTATION) !=
            mAvailableResults.end();
    mReportPoseTranslation = mAvailableResults.find(ANDROID_LENS_POSE_TRANSLATION) !=
            mAvailableResults.end();
    mReportIntrinsicCalibration = mAvailableResults.find(ANDROID_LENS_INTRINSIC_CALIBRATION) !=
            mAvailableResults.end();
    mReportDistortion = mAvailableResults.find(ANDROID_LENS_DISTORTION) != mAvailableResults.end();

    mFocusDistance = mMinimumFocusDistance;
    for (size_t idx = 0; idx < kTemplateCount; idx++) {
        if (mDefaultRequests[idx].get() == nullptr) {
            continue;
        }

        mDefaultRequests[idx]->Set(ANDROID_LENS_APERTURE, &mAperture, 1);
        mDefaultRequests[idx]->Set(ANDROID_LENS_FOCAL_LENGTH, &mFocalLength, 1);
        mDefaultRequests[idx]->Set(ANDROID_LENS_FOCUS_DISTANCE, &mFocusDistance, 1);
        mDefaultRequests[idx]->Set(ANDROID_LENS_OPTICAL_STABILIZATION_MODE, &mOISMode, 1);
    }

    return initializeFlashDefaults();
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

    return initializeLensDefaults();
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
    mSupportsManualPostProcessing = supportsCapability(
            ANDROID_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_POST_PROCESSING);
    mIsBackwardCompatible = supportsCapability(
            ANDROID_REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE);
    mIsRAWCapable = supportsCapability(ANDROID_REQUEST_AVAILABLE_CAPABILITIES_RAW);

    if (mSupportsManualSensor) {
        auto templateIdx = static_cast<size_t> (RequestTemplate::kManual);
        mDefaultRequests[templateIdx] = HalCameraMetadata::Create(1, 10);
    }

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
