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

#ifndef EMULATOR_CAMERA_HAL_HWL_REQUEST_STATE_H
#define EMULATOR_CAMERA_HAL_HWL_REQUEST_STATE_H

#include "EmulatedSensor.h"
#include "hwl_types.h"
#include <mutex>
#include <unordered_map>

namespace android {

using google_camera_hal::HalCameraMetadata;
using google_camera_hal::HalStream;
using google_camera_hal::HwlPipelineCallback;
using google_camera_hal::HwlPipelineRequest;
using google_camera_hal::RequestTemplate;
using google_camera_hal::StreamBuffer;

struct PendingRequest;

class EmulatedRequestState {
public:
    EmulatedRequestState(uint32_t cameraId) : mCameraId(cameraId) {}
    virtual ~EmulatedRequestState() {}

    status_t initialize(std::unique_ptr<HalCameraMetadata> staticMeta);

    status_t getDefaultRequest(RequestTemplate type,
            std::unique_ptr<HalCameraMetadata>* default_settings/*out*/);

    std::unique_ptr<HwlPipelineResult> initializeResult(const PendingRequest& request,
            uint32_t pipelineId, uint32_t frameNumber);

    status_t initializeSensorSettings(std::unique_ptr<HalCameraMetadata> requestSettings,
            EmulatedSensor::SensorSettings *sensorSettings/*out*/);

    // Helper methods
    uint8_t getMaxPipelineDepth() const { return mMaxPipelineDepth; }

private:

    bool supportsCapability(uint8_t cap);

    status_t initializeRequestDefaults();
    status_t initializeSensorDefaults();
    status_t initializeFlashDefaults();
    status_t initializeControlDefaults();
    status_t initializeControlAEDefaults();
    status_t initializeControlAWBDefaults();
    status_t initializeControlAFDefaults();
    status_t initializeControlSceneDefaults();
    status_t initializeHotPixelDefaults();
    status_t initializeStatisticsDefaults();
    status_t initializeTonemapDefaults();
    status_t initializeBlackLevelDefaults();
    status_t initializeEdgeDefaults();
    status_t initializeShadingDefaults();
    status_t initializeNoiseReductionDefaults();
    status_t initializeColorCorrectionDefaults();
    status_t initializeScalerDefaults();
    status_t initializeMeteringRegionDefault(uint32_t tag, int32_t *region/*out*/);
    status_t initializeControlefaults();
    status_t initializeInfoDefaults();
    status_t initializeLensDefaults();

    status_t processAE();
    status_t processAF();
    status_t processAWB();
    status_t doFakeAE();
    status_t compensateAE();
    status_t update3AMeteringRegion(uint32_t tag, const HalCameraMetadata& settings,
            int32_t *region /*out*/);

    std::mutex mRequestStateMutex;
    std::unique_ptr<HalCameraMetadata> mRequestSettings;

    // Supported capabilities and features
    static const std::set<uint8_t> kSupportedCapabilites;
    static const std::set<uint8_t> kSupportedHWLevels;
    std::unique_ptr<HalCameraMetadata> mStaticMetadata;

    // android.blacklevel.*
    uint8_t mBlackLevelLock = ANDROID_BLACK_LEVEL_LOCK_ON;
    bool mReportBlackLevelLock = false;

    // android.colorcorrection.*
    std::set<uint8_t> mAvailableColorAberrationModes;

    // android.edge.*
    std::set<uint8_t> mAvailableEdgeModes;

    // android.shading.*
    std::set<uint8_t> mAvailableShadingModes;

    // android.noiseReduction.*
    std::set<uint8_t> mAvailableNoiseReductionModes;

    // android.request.*
    std::set<uint8_t> mAvailableCapabilites;
    std::set<int32_t> mAvailableCharacteritics;
    std::set<int32_t> mAvailableResults;
    std::set<int32_t> mAvailableRequests;
    uint8_t mMaxPipelineDepth = 0;
    int32_t mPartialResultCount = 1; // TODO: add support for partial results
    bool mSupportsManualSensor = false;
    bool mSupportsManualPostProcessing = false;
    bool mIsBackwardCompatible = false;
    bool mIsRAWCapable = false;

    // android.control.*
    struct SceneOverride {
        uint8_t aeMode, awbMode, afMode;
        SceneOverride() : aeMode(ANDROID_CONTROL_AE_MODE_OFF),
                awbMode(ANDROID_CONTROL_AWB_MODE_OFF), afMode(ANDROID_CONTROL_AF_MODE_OFF) {}
        SceneOverride(uint8_t ae, uint8_t awb, uint8_t af) : aeMode(ae), awbMode(awb), afMode(af) {}
    };

    struct FPSRange {
        int32_t minFPS, maxFPS;
        FPSRange() : minFPS(-1), maxFPS(-1) {}
        FPSRange(int32_t min, int32_t max) : minFPS(min), maxFPS(max) {}
    };

    std::set<uint8_t> mAvailableControlModes;
    std::set<uint8_t> mAvailableAEModes;
    std::set<uint8_t> mAvailableAFModes;
    std::set<uint8_t> mAvailableAWBModes;
    std::set<uint8_t> mAvailableScenes;
    std::set<uint8_t> mAvailableAntibandingModes;
    std::set<uint8_t> mAvailableEffects;
    std::set<uint8_t> mAvailableVSTABModes;
    std::unordered_map<uint8_t, SceneOverride> mSceneOverrides;
    std::vector<FPSRange> mAvailableFPSRanges;
    int32_t mExposureCompensationRange [2] = {0, 0};
    camera_metadata_rational mExposureCompensationStep = {0, 0};
    bool mExposureCompensationSupported = false;
    int32_t mExposureCompensation = 0;
    int32_t mAEMeteringRegion[5] = {0, 0, 0, 0, 0};
    int32_t mAWBMeteringRegion[5] = {0, 0, 0, 0, 0};
    int32_t mAFMeteringRegion[5] = {0, 0, 0, 0, 0};
    size_t mMaxAERegions = 0;
    size_t mMaxAWBRegions = 0;
    size_t mMaxAFRegions = 0;
    uint8_t mControlMode = ANDROID_CONTROL_MODE_AUTO;
    uint8_t mSceneMode = ANDROID_CONTROL_SCENE_MODE_DISABLED;
    uint8_t mAEMode = ANDROID_CONTROL_AE_MODE_ON;
    uint8_t mAWBMode = ANDROID_CONTROL_AWB_MODE_AUTO;
    uint8_t mAFMode = ANDROID_CONTROL_AF_MODE_AUTO;
    uint8_t mAELock = ANDROID_CONTROL_AE_LOCK_OFF;
    uint8_t mAEState = ANDROID_CONTROL_AE_STATE_INACTIVE;
    uint8_t mAWBState = ANDROID_CONTROL_AWB_STATE_INACTIVE;
    uint8_t mAWBLock = ANDROID_CONTROL_AWB_LOCK_OFF;
    uint8_t mAFState = ANDROID_CONTROL_AF_STATE_INACTIVE;
    uint8_t mAFTrigger = ANDROID_CONTROL_AF_TRIGGER_IDLE;
    uint8_t mAETrigger = ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_IDLE;
    FPSRange mAETargetFPS = {0, 0};
    static const int32_t kMinimumStreamingFPS = 20;
    bool mAELockAvailable = false;
    bool mReportAELock = false;
    bool mScenesSupported = false;
    size_t mAEFrameCounter = 0;
    const size_t kAEPrecaptureMinFrames = 10;
    const float kExposureTrackRate = .2f;
    const size_t kStableAeMaxFrames = 100;
    const float kExposureWanderMin = -2;
    const float kExposureWanderMax = 1;
    int32_t mPostRawBoost = 100;
    bool mReportPostRawBoost = false;
    nsecs_t mAETargetExposureTime = EmulatedSensor::kDefaultExposureTime;
    nsecs_t mCurrentExposureTime = EmulatedSensor::kDefaultExposureTime;
    bool mAWBLockAvailable = false;
    bool mReportAWBLock = false;
    bool mAFModeChanged = false;
    bool mAFSupported = false;
    bool mPictureCAFSupported = false;
    bool mVideoCAFSupported = false;

    // android.flash.*
    bool mIsFlashSupported = false;
    uint8_t mFlashState = ANDROID_FLASH_STATE_UNAVAILABLE;

    // android.sensor.*
    std::pair<int32_t, int32_t> mSensorSensitivityRange;
    std::pair<nsecs_t, nsecs_t> mSensorExposureTimeRange;
    nsecs_t mSensorMaxFrameDuration = EmulatedSensor::kSupportedFrameDurationRange[1];
    nsecs_t mSensorExposureTime = EmulatedSensor::kDefaultExposureTime;
    nsecs_t mSensorFrameDuration = EmulatedSensor::kDefaultFrameDuration;
    int32_t mSensorSensitivity = EmulatedSensor::kDefaultSensitivity;
    bool mReportSensorSettings = false;
    std::set<int32_t> mAvailableTestPatternModes;
    bool mReportRollingShutterSkew = false;

    // android.scaler.*
    int32_t mScalerCropRegionDefault[4] = {0, 0, 0, 0};

    // android.statistics.*
    std::set<uint8_t> mAvailableHotPixelMapModes;
    std::set<uint8_t> mAvailableLensShadingMapModes;
    std::set<uint8_t> mAvailableFaceDetectModes;
    uint8_t mCurrentSceneFlicker = ANDROID_STATISTICS_SCENE_FLICKER_NONE;
    bool mReportSceneFlicker = false;

    // android.tonemap.*
    std::set<uint8_t> mAvailableTonemapModes;

    // android.info.*
    uint8_t mSupportedHWLevel = 0;
    static const size_t kTemplateCount = static_cast<size_t>(RequestTemplate::kManual) + 1;
    std::unique_ptr<HalCameraMetadata> mDefaultRequests[kTemplateCount];

    // android.lens.*
    float mMinimumFocusDistance = 0.f;
    float mAperture = 0.f;
    float mFocalLength = 0.f;
    float mFocusDistance = 0.f;
    bool mReportFocusDistance = false;
    uint8_t mLensState = ANDROID_LENS_STATE_STATIONARY;
    bool mReportFocusRange = false;
    float mFilterDensity = 0.f;
    bool mReportFilterDensity = false;
    std::set<uint8_t> mAvailableOISModes;
    uint8_t mOISMode = ANDROID_LENS_OPTICAL_STABILIZATION_MODE_OFF;
    bool mReportOISMode = false;

    //android.hotpixel.*
    std::set<uint8_t> mAvailableHotPixelModes;

    uint32_t mCameraId;

    EmulatedRequestState(const EmulatedRequestState&) = delete;
    EmulatedRequestState& operator = (const EmulatedRequestState&) = delete;
};

}  // namespace android

#endif  // EMULATOR_CAMERA_HAL_HWL_REQUEST_STATE_H
