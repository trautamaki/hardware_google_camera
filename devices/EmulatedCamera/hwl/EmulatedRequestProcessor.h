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

#ifndef EMULATOR_CAMERA_HAL_HWL_REQUEST_PROCESSOR_H
#define EMULATOR_CAMERA_HAL_HWL_REQUEST_PROCESSOR_H

#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>

#include "EmulatedSensor.h"
#include "hwl_types.h"

namespace android {

using google_camera_hal::HalCameraMetadata;
using google_camera_hal::HalStream;
using google_camera_hal::HwlPipelineCallback;
using google_camera_hal::HwlPipelineRequest;
using google_camera_hal::RequestTemplate;
using google_camera_hal::StreamBuffer;

struct EmulatedStream : public HalStream {
    uint32_t width, height;
    size_t bufferSize;
};

struct EmulatedPipeline {
    HwlPipelineCallback cb;
    // stream id -> stream map
    std::unordered_map<uint32_t, EmulatedStream> streams;
    uint32_t physicalCameraId, pipelineId;
};

class EmulatedRequestProcessor {
public:
    EmulatedRequestProcessor(uint32_t cameraId, uint8_t maxPipelineDepth,
            sp<EmulatedSensor> sensor);
    virtual ~EmulatedRequestProcessor();

    // Process given pipeline requests and invoke the respective callback in a separate thread
    status_t processPipelineRequests(uint32_t frameNumber,
            const std::vector<HwlPipelineRequest>& requests,
            const std::vector<EmulatedPipeline>& pipelines);

    status_t retrieveDefaultRequest(RequestTemplate type,
            std::unique_ptr<HalCameraMetadata>* default_settings);

    status_t flush();

    status_t initialize(std::unique_ptr<HalCameraMetadata> staticMeta);

private:

    void requestProcessorLoop();

    std::mutex mProcessMutex;
    std::condition_variable mRequestCondition;
    std::thread mRequestThread;
    bool mProcessorDone = false;

    struct PendingRequest {
        std::unique_ptr<HalCameraMetadata> settings;
        std::vector<StreamBuffer> inputBuffers;
        // TODO: input buffer meta
        std::unique_ptr<Buffers> outputBuffers;
    };

    // helper methods
    static uint32_t inline alignTo(uint32_t value, uint32_t alignment) {
        uint32_t delta = value % alignment;
        return (delta == 0) ? value : (value + (alignment - delta));
    }

    bool supportsCapability(uint8_t cap);
    bool isBackwardCompatible();
    status_t getBufferSizeAndStride(const EmulatedStream& stream, uint32_t *size /*out*/,
            uint32_t *stride /*out*/);
    status_t lockSensorBuffer(const EmulatedStream& stream, HandleImporter& importer /*in*/,
            buffer_handle_t buffer, SensorBuffer *sensorBuffer /*out*/);
    std::unique_ptr<SensorBuffer> createSensorBuffer(uint32_t frameNumber,
            const EmulatedStream& stream, const HwlPipelineRequest& request,
            HwlPipelineCallback callback, StreamBuffer streamBuffer);
    std::unique_ptr<Buffers> initializeOutputBuffers(const PendingRequest& request);
    std::unique_ptr<HwlPipelineResult> initializeResultLocked(const PendingRequest& request,
            uint32_t pipelineId, uint32_t frameNumber);
    status_t initializeSensorSettingsLocked(const PendingRequest& request,
            EmulatedSensor::SensorSettings *settings/*out*/);
    void notifyFailedRequest(const PendingRequest& request);

    status_t initializeDefaultRequests();
    status_t initializeSensorDefaults();
    status_t initializeControlDefaults();
    status_t initializeInfoDefaults();


    std::unique_ptr<HalCameraMetadata> mStaticMetadata;

    // Supported capabilities and features
    static const std::set<uint8_t> kSupportedCapabilites;
    static const std::set<uint8_t> kSupportedHWLevels;

    // android.request.*
    std::set<uint8_t> mAvailableCapabilites;
    std::set<int32_t> mAvailableCharacteritics;
    std::set<int32_t> mAvailableResults;
    std::set<int32_t> mAvailableRequests;
    // android.control.*
    std::set<uint8_t> mAvailableControlModes;
    std::set<uint8_t> mAvailableAEModes;
    std::set<uint8_t> mAvailableAFModes;
    std::set<uint8_t> mAvailableAWBModes;
    uint8_t mControlMode = ANDROID_CONTROL_MODE_AUTO;
    uint8_t mAEMode = ANDROID_CONTROL_AE_MODE_ON;
    // android.sensor.*
    std::pair<int32_t, int32_t> mSensorSensitivityRange;
    std::pair<nsecs_t, nsecs_t> mSensorExposureTimeRange;
    nsecs_t mSensorMaxFrameDuration = EmulatedSensor::kSupportedFrameDurationRange[1];
    nsecs_t mSensorExposureTime = EmulatedSensor::kDefaultExposureTime;
    nsecs_t mSensorFrameDuration = EmulatedSensor::kDefaultFrameDuration;
    int32_t mSensorSensitivity = EmulatedSensor::kDefaultSensitivity;
    // android.info.*
    uint8_t mSupportedHWLevel = 0;
    static const size_t kTemplateCount = static_cast<size_t>(RequestTemplate::kManual) + 1;
    std::unique_ptr<HalCameraMetadata> mDefaultRequests[kTemplateCount];

    std::queue<PendingRequest> mPendingRequests;
    uint8_t mMaxPipelineDepth;
    uint32_t mCameraId;
    sp<EmulatedSensor> mSensor;

    EmulatedRequestProcessor(const EmulatedRequestProcessor&) = delete;
    EmulatedRequestProcessor& operator = (const EmulatedRequestProcessor&) = delete;
};

}  // namespace android

#endif  // EMULATOR_CAMERA_HAL_HWL_REQUEST_PROCESSOR_H
