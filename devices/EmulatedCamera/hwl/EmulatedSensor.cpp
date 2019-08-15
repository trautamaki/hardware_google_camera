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

//#define LOG_NDEBUG 0
//#define LOG_NNDEBUG 0
#define LOG_TAG "EmulatedSensor"
#define ATRACE_TAG ATRACE_TAG_CAMERA

#ifdef LOG_NNDEBUG
#define ALOGVV(...) ALOGV(__VA_ARGS__)
#else
#define ALOGVV(...) ((void)0)
#endif

#include <utils/Log.h>
#include <utils/Trace.h>

#include <cmath>
#include <cstdlib>
#include "EmulatedSensor.h"
#include <inttypes.h>
#include <libyuv.h>
#include <system/camera_metadata.h>
#include "utils/ExifUtils.h"
#include "utils/HWLUtils.h"

namespace android {

using google_camera_hal::HalCameraMetadata;
using google_camera_hal::NotifyMessage;
using google_camera_hal::MessageType;

 // 1 us - 30 sec
const nsecs_t EmulatedSensor::kSupportedExposureTimeRange[2] = {1000LL, 30000000000LL} ;

 // ~1/30 s - 30 sec
const nsecs_t EmulatedSensor::kSupportedFrameDurationRange[2] = {33331760LL, 30000000000LL};

const int32_t EmulatedSensor::kSupportedSensitivityRange[2] = {100, 1600};
const int32_t EmulatedSensor::kDefaultSensitivity = 100; // ISO
const nsecs_t EmulatedSensor::kDefaultExposureTime = ms2ns(15);
const nsecs_t EmulatedSensor::kDefaultFrameDuration = ms2ns(33);

// Sensor defaults
const uint8_t EmulatedSensor::kSupportedColorFilterArrangement =
        ANDROID_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_RGGB;
const uint32_t EmulatedSensor::kDefaultMaxRawValue = 4000;
const uint32_t EmulatedSensor::kDefaultBlackLevelPattern[4] = {1000, 1000, 1000, 1000};

const nsecs_t EmulatedSensor::kMinVerticalBlank = 10000L;

// Sensor sensitivity
const float EmulatedSensor::kSaturationVoltage = 0.520f;
const uint32_t EmulatedSensor::kSaturationElectrons = 2000;
const float EmulatedSensor::kVoltsPerLuxSecond = 0.100f;

const float EmulatedSensor::kElectronsPerLuxSecond = EmulatedSensor::kSaturationElectrons /
        EmulatedSensor::kSaturationVoltage * EmulatedSensor::kVoltsPerLuxSecond;

const float EmulatedSensor::kReadNoiseStddevBeforeGain = 1.177;  // in electrons
const float EmulatedSensor::kReadNoiseStddevAfterGain = 2.100;   // in digital counts
const float EmulatedSensor::kReadNoiseVarBeforeGain =
        EmulatedSensor::kReadNoiseStddevBeforeGain * EmulatedSensor::kReadNoiseStddevBeforeGain;
const float EmulatedSensor::kReadNoiseVarAfterGain =
        EmulatedSensor::kReadNoiseStddevAfterGain * EmulatedSensor::kReadNoiseStddevAfterGain;

const uint32_t EmulatedSensor::kMaxRAWStreams = 1;
const uint32_t EmulatedSensor::kMaxProcessedStreams = 3;
const uint32_t EmulatedSensor::kMaxStallingStreams = 2;
const uint32_t EmulatedSensor::kMaxInputStreams = 1;

const uint32_t EmulatedSensor::kMaxLensShadingMapSize[2] {64, 64};
const int32_t EmulatedSensor::kFixedBitPrecision = 64; // 6-bit
// In fixed-point math, saturation point of sensor after gain
const int32_t EmulatedSensor::kSaturationPoint = kFixedBitPrecision * 255;
const camera_metadata_rational EmulatedSensor::kNeutralColorPoint[3] =
        {{255, 1}, {255, 1}, {255, 1}};
const float EmulatedSensor::kGreenSplit = 1.f; // No divergence
// Reduce memory usage by allowing only one buffer in sensor, one in jpeg compressor
// and one pending request to avoid stalls.
const uint8_t EmulatedSensor::kPipelineDepth = 3;

const camera_metadata_rational EmulatedSensor::kDefaultColorTransform[9] =
        {{1, 1}, {0, 1}, {0, 1}, {0, 1}, {1, 1}, {0, 1}, {0, 1}, {0, 1}, {1, 1}};
const float EmulatedSensor::kDefaultColorCorrectionGains[4] = {1.0f, 1.0f, 1.0f, 1.0f};

const float EmulatedSensor::kDefaultToneMapCurveRed[4] = {.0f, .0f, 1.f, 1.f};
const float EmulatedSensor::kDefaultToneMapCurveGreen[4] = {.0f, .0f, 1.f, 1.f};
const float EmulatedSensor::kDefaultToneMapCurveBlue[4] = {.0f, .0f, 1.f, 1.f};

/** A few utility functions for math, normal distributions */

// Take advantage of IEEE floating-point format to calculate an approximate
// square root. Accurate to within +-3.6%
float sqrtf_approx(float r) {
    // Modifier is based on IEEE floating-point representation; the
    // manipulations boil down to finding approximate log2, dividing by two, and
    // then inverting the log2. A bias is added to make the relative error
    // symmetric about the real answer.
    const int32_t modifier = 0x1FBB4000;

    int32_t r_i = *(int32_t *)(&r);
    r_i = (r_i >> 1) + modifier;

    return *(float *)(&r_i);
}

EmulatedSensor::EmulatedSensor() : Thread(false), mGotVSync(false) {
    mGammaTable.resize(kSaturationPoint + 1);
    for (int32_t i = 0; i <= kSaturationPoint; i++) {
        mGammaTable[i] = applysRGBGamma(i, kSaturationPoint);
    }
}

EmulatedSensor::~EmulatedSensor() {
    shutDown();
}

bool EmulatedSensor::areCharacteristicsSupported(const SensorCharacteristics& characteristics) {
    if ((characteristics.width == 0) || (characteristics.height == 0)) {
        ALOGE("%s: Invalid sensor size %zux%zu", __FUNCTION__,
                characteristics.width, characteristics.height);
        return false;
    }

    if ((characteristics.exposureTimeRange[0] >= characteristics.exposureTimeRange[1]) ||
            ((characteristics.exposureTimeRange[0] < kSupportedExposureTimeRange[0]) ||
             (characteristics.exposureTimeRange[1] > kSupportedExposureTimeRange[1]))) {
        ALOGE("%s: Unsupported exposure range", __FUNCTION__);
        return false;
    }

    if ((characteristics.frameDurationRange[0] >= characteristics.frameDurationRange[1]) ||
            ((characteristics.frameDurationRange[0] < kSupportedFrameDurationRange[0]) ||
             (characteristics.frameDurationRange[1] > kSupportedFrameDurationRange[1]))) {
        ALOGE("%s: Unsupported frame duration range", __FUNCTION__);
        return false;
    }

    if ((characteristics.sensitivityRange[0] >= characteristics.sensitivityRange[1]) ||
            ((characteristics.sensitivityRange[0] < kSupportedSensitivityRange[0]) ||
             (characteristics.sensitivityRange[1] > kSupportedSensitivityRange[1])) ||
            (!((kDefaultSensitivity >= characteristics.sensitivityRange[0]) &&
               (kDefaultSensitivity <= characteristics.sensitivityRange[1])))) {
        ALOGE("%s: Unsupported sensitivity range", __FUNCTION__);
        return false;
    }

    if (characteristics.colorArangement != kSupportedColorFilterArrangement) {
        ALOGE("%s: Unsupported color arrangement!", __FUNCTION__);
        return false;
    }

    for (const auto& blackLevel : characteristics.blackLevelPattern) {
        if (blackLevel >= characteristics.maxRawValue) {
            ALOGE("%s: Black level matches or exceeds max RAW value!", __FUNCTION__);
            return false;
        }
    }

    if ((characteristics.frameDurationRange[0] / characteristics.height) == 0) {
        ALOGE("%s: Zero row readout time!", __FUNCTION__);
        return false;
    }

    if (characteristics.maxRawStreams > kMaxRAWStreams) {
        ALOGE("%s: RAW streams maximum %u exceeds supported maximum %u", __FUNCTION__,
                characteristics.maxRawStreams, kMaxRAWStreams);
        return false;
    }

    if (characteristics.maxProcessedStreams > kMaxProcessedStreams) {
        ALOGE("%s: Processed streams maximum %u exceeds supported maximum %u", __FUNCTION__,
                characteristics.maxProcessedStreams, kMaxProcessedStreams);
        return false;
    }

    if (characteristics.maxStallingStreams > kMaxStallingStreams) {
        ALOGE("%s: Stalling streams maximum %u exceeds supported maximum %u", __FUNCTION__,
                characteristics.maxStallingStreams, kMaxStallingStreams);
        return false;
    }

    if (characteristics.maxInputStreams > kMaxInputStreams) {
        ALOGE("%s: Input streams maximum %u exceeds supported maximum %u", __FUNCTION__,
                characteristics.maxInputStreams, kMaxInputStreams);
        return false;
    }

    if ((characteristics.lensShadingMapSize[0] > kMaxLensShadingMapSize[0]) ||
            (characteristics.lensShadingMapSize[1] > kMaxLensShadingMapSize[1])) {
        ALOGE("%s: Lens shading map [%dx%d] exceeds supprorted maximum [%dx%d]", __FUNCTION__,
                characteristics.lensShadingMapSize[0], characteristics.lensShadingMapSize[1],
                kMaxLensShadingMapSize[0], kMaxLensShadingMapSize[1]);
        return false;
    }

    if (characteristics.maxPipelineDepth < kPipelineDepth) {
        ALOGE("%s: Pipeline depth %d smaller than supprorted minimum %d", __FUNCTION__,
                characteristics.maxPipelineDepth, kPipelineDepth);
        return false;
    }

    return true;
}

bool EmulatedSensor::isStreamCombinationSupported(const StreamConfiguration& config,
        StreamConfigurationMap& map, const SensorCharacteristics& sensorChars) {
    uint32_t rawStreamCount = 0;
    uint32_t inputStreamCount = 0;
    uint32_t processedStreamCount = 0;
    uint32_t stallingStreamCount = 0;

    for (const auto& stream : config.streams) {
        if (stream.rotation != google_camera_hal::StreamRotation::kRotation0) {
            ALOGE("%s: Stream rotation: 0x%x not supported!", __FUNCTION__, stream.rotation);
            return false;
        }

        if (stream.stream_type == google_camera_hal::StreamType::kInput) {
            if (sensorChars.maxInputStreams == 0) {
                ALOGE("%s: Input streams are not supported on this device!", __FUNCTION__);
                return false;
            }

            auto supportedOutputs = map.getValidOutputFormatsForInput(stream.format);
            if (supportedOutputs.empty()) {
                ALOGE("%s: Input stream with format: 0x%x no supported on this device!",
                        __FUNCTION__, stream.format);
                return false;
            }

            inputStreamCount++;
        } else {
            switch (stream.format) {
                case HAL_PIXEL_FORMAT_BLOB:
                    if (stream.data_space != HAL_DATASPACE_V0_JFIF) {
                        ALOGE("%s: Unsupported Blob dataspace 0x%x", __FUNCTION__,
                                stream.data_space);
                        return false;
                    }
                    stallingStreamCount++;
                    break;
                case HAL_PIXEL_FORMAT_RAW16:
                    rawStreamCount++;
                    break;
                default:
                    processedStreamCount++;
            }
        }

        auto outputSizes = map.getOutputSizes(stream.format);
        if (outputSizes.empty()) {
            ALOGE("%s: Unsupported format: 0x%x", __FUNCTION__, stream.format);
            return false;
        }

        auto streamSize = std::make_pair(stream.width, stream.height);
        if (outputSizes.find(streamSize) == outputSizes.end()) {
            ALOGE("%s: Stream with size %dx%d and format 0x%x is not supported!", __FUNCTION__,
                    stream.width, stream.height, stream.format);
            return false;
        }
    }

    if (rawStreamCount > sensorChars.maxRawStreams) {
        ALOGE("%s: RAW streams maximum %u exceeds supported maximum %u", __FUNCTION__,
                rawStreamCount, sensorChars.maxRawStreams);
        return false;
    }

    if (processedStreamCount > sensorChars.maxProcessedStreams) {
        ALOGE("%s: Processed streams maximum %u exceeds supported maximum %u", __FUNCTION__,
                processedStreamCount, sensorChars.maxProcessedStreams);
        return false;
    }

    if (stallingStreamCount > sensorChars.maxStallingStreams) {
        ALOGE("%s: Stalling streams maximum %u exceeds supported maximum %u", __FUNCTION__,
                stallingStreamCount, sensorChars.maxStallingStreams);
        return false;
    }

    if (inputStreamCount > sensorChars.maxInputStreams) {
        ALOGE("%s: Input stream maximum %u exceeds supported maximum %u", __FUNCTION__,
                inputStreamCount, sensorChars.maxInputStreams);
        return false;
    }

    return true;
}

status_t EmulatedSensor::startUp(SensorCharacteristics characteristics) {
    ALOGV("%s: E", __FUNCTION__);

    if (!areCharacteristicsSupported(characteristics)) {
        ALOGE("%s: Sensor characteristics not supported!", __FUNCTION__);
        return BAD_VALUE;
    }

    std::unique_ptr<ExifUtils> exifUtils(ExifUtils::create(characteristics));

    if (isRunning()) {
        return OK;
    }

    mChars = characteristics;
    mScene = std::make_unique<EmulatedScene>(mChars.width, mChars.height, kElectronsPerLuxSecond);
    mScene->setColorFilterXYZ(
            mChars.colorFilter.rX,
            mChars.colorFilter.rY,
            mChars.colorFilter.rZ,
            mChars.colorFilter.grX,
            mChars.colorFilter.grY,
            mChars.colorFilter.grZ,
            mChars.colorFilter.gbX,
            mChars.colorFilter.gbY,
            mChars.colorFilter.gbZ,
            mChars.colorFilter.bX,
            mChars.colorFilter.bY,
            mChars.colorFilter.bZ);
    mRowReadoutTime = mChars.frameDurationRange[0] / mChars.height;
    kBaseGainFactor = (float)mChars.maxRawValue / EmulatedSensor::kSaturationElectrons;
    mJpegCompressor = std::make_unique<JpegCompressor>(std::move(exifUtils));

    if ((mChars.lensShadingMapSize[0] > 0) && (mChars.lensShadingMapSize[1] > 0)) {
        // Perfect lens, no actual shading needed.
        mLensShadingMap.resize(
                mChars.lensShadingMapSize[0] * mChars.lensShadingMapSize[1] * 4, 1.f);
    }

    auto res = run(LOG_TAG, ANDROID_PRIORITY_URGENT_DISPLAY);
    if (res != OK) {
        ALOGE("Unable to start up sensor capture thread: %d", res);
    }
    return res;
}

status_t EmulatedSensor::shutDown() {
    int res;
    res = requestExitAndWait();
    if (res != OK) {
        ALOGE("Unable to shut down sensor capture thread: %d", res);
    }
    return res;
}

void EmulatedSensor::setCurrentRequest(SensorSettings settings,
        std::unique_ptr<HwlPipelineResult> result,
        std::unique_ptr<Buffers> inputBuffers, std::unique_ptr<Buffers> outputBuffers) {
    Mutex::Autolock lock(mControlMutex);
    mCurrentSettings = settings;
    mCurrentResult = std::move(result);
    mCurrentInputBuffers = std::move(inputBuffers);
    mCurrentOutputBuffers = std::move(outputBuffers);
}

bool EmulatedSensor::waitForVSyncLocked(nsecs_t reltime) {
    mGotVSync = false;
    auto res = mVSync.waitRelative(mControlMutex, reltime);
    if (res != OK && res != TIMED_OUT) {
        ALOGE("%s: Error waiting for VSync signal: %d", __FUNCTION__, res);
        return false;
    }

    return mGotVSync;
}

bool EmulatedSensor::waitForVSync(nsecs_t reltime) {
    Mutex::Autolock lock(mControlMutex);

    return waitForVSyncLocked(reltime);
}

status_t EmulatedSensor::flush() {
    Mutex::Autolock lock(mControlMutex);
    auto ret = waitForVSyncLocked(kSupportedFrameDurationRange[1]);

    // First recreate the jpeg compressor. This will abort any ongoing processing and
    // flush any pending jobs.
    std::unique_ptr<ExifUtils> exifUtils(ExifUtils::create(mChars));
    mJpegCompressor = std::make_unique<JpegCompressor>(std::move(exifUtils));

    // Then return any pending frames here
    if ((mCurrentInputBuffers.get() != nullptr) && (!mCurrentInputBuffers->empty())) {
        mCurrentInputBuffers->clear();
    }
    if ((mCurrentOutputBuffers.get() != nullptr) && (!mCurrentOutputBuffers->empty())) {
        for (const auto& buffer : *mCurrentOutputBuffers) {
            buffer->streamBuffer.status = BufferStatus::kError;
        }

        if ((mCurrentResult.get() != nullptr) &&
                (mCurrentResult->result_metadata.get() != nullptr)) {
            if (mCurrentOutputBuffers->at(0)->callback.notify != nullptr) {
                NotifyMessage msg {
                    .type = MessageType::kError,
                        .message.error = {
                            .frame_number = mCurrentOutputBuffers->at(0)->frameNumber,
                            .error_stream_id = -1,
                            .error_code = ErrorCode::kErrorResult,
                        }
                };

                mCurrentOutputBuffers->at(0)->callback.notify(mCurrentResult->pipeline_id, msg);
            }
        }

        mCurrentOutputBuffers->clear();
    }

    return ret ? OK : TIMED_OUT;
}

bool EmulatedSensor::threadLoop() {
    ATRACE_CALL();
    /**
     * Sensor capture operation main loop.
     *
     */

    /**
     * Stage 1: Read in latest control parameters
     */
    std::unique_ptr<Buffers> nextBuffers;
    std::unique_ptr<Buffers> nextInputBuffer;
    std::unique_ptr<HwlPipelineResult> nextResult;
    SensorSettings settings;
    HwlPipelineCallback callback = {nullptr, nullptr};
    {
        Mutex::Autolock lock(mControlMutex);
        settings = mCurrentSettings;
        std::swap(nextBuffers, mCurrentOutputBuffers);
        std::swap(nextInputBuffer, mCurrentInputBuffers);
        std::swap(nextResult, mCurrentResult);

        // Signal VSync for start of readout
        ALOGVV("Sensor VSync");
        mGotVSync = true;
        mVSync.signal();
    }

    if (settings.frameDuration == 0) {
        settings.frameDuration = EmulatedSensor::kSupportedFrameDurationRange[0];
    }
    nsecs_t startRealTime = systemTime();
    // Stagefright cares about system time for timestamps, so base simulated
    // time on that.
    nsecs_t frameEndRealTime = startRealTime + settings.frameDuration;

    /**
     * Stage 2: Capture new image
     */
    mNextCaptureTime = frameEndRealTime;

    bool reprocessRequest = false;
    if ((nextInputBuffer.get() != nullptr) && (!nextInputBuffer->empty())) {
        if (nextInputBuffer->size() > 1) {
            ALOGW("%s: Reprocess supports only single input!", __FUNCTION__);
        }
        if (nextInputBuffer->at(0)->format != HAL_PIXEL_FORMAT_YCBCR_420_888) {
            ALOGE("%s: Reprocess input format: 0x%x not supported! Skipping reprocess!",
                    __FUNCTION__, nextInputBuffer->at(0)->format);
        } else {
            camera_metadata_ro_entry_t entry;
            auto ret = nextResult->result_metadata->Get(ANDROID_SENSOR_TIMESTAMP, &entry);
            if ((ret == OK) && (entry.count == 1)) {
                mNextCaptureTime = entry.data.i64[0];
            } else {
                ALOGW("%s: Reprocess timestamp absent!", __FUNCTION__);
            }

            reprocessRequest = true;
        }
    }

    if (nextBuffers != nullptr) {
        callback = nextBuffers->at(0)->callback;
        if (callback.notify != nullptr) {
            NotifyMessage msg {
                .type = MessageType::kShutter,
                .message.shutter = {
                    .frame_number = nextBuffers->at(0)->frameNumber,
                    .timestamp_ns = static_cast<uint64_t>(mNextCaptureTime)
                }
            };
            callback.notify(nextResult->pipeline_id, msg);
        }
        ALOGVV("Starting next capture: Exposure: %f ms, gain: %d", ns2ms(settings.exposureTime),
                gain);
        mScene->setExposureDuration((float)settings.exposureTime / 1e9);
        mScene->calculateScene(mNextCaptureTime);
        nextResult->result_metadata->Set(ANDROID_SENSOR_TIMESTAMP, &mNextCaptureTime, 1);
        if (settings.lensShadingMapMode == ANDROID_STATISTICS_LENS_SHADING_MAP_MODE_ON) {
            nextResult->result_metadata->Set(ANDROID_STATISTICS_LENS_SHADING_MAP,
                    mLensShadingMap.data(), mLensShadingMap.size());
        }
        if (settings.reportNeutralColorPoint) {
            nextResult->result_metadata->Set(ANDROID_SENSOR_NEUTRAL_COLOR_POINT, kNeutralColorPoint,
                    ARRAY_SIZE(kNeutralColorPoint));
        }
        if (settings.reportGreenSplit) {
            nextResult->result_metadata->Set(ANDROID_SENSOR_GREEN_SPLIT, &kGreenSplit, 1);
        }
        if (settings.reportNoiseProfile) {
            // TODO: pass the results as parameter to capture instead of re-calculating
            float totalGain = settings.gain / 100.0 * kBaseGainFactor;
            float noiseVarGain = totalGain * totalGain;
            float readNoiseVar = kReadNoiseVarBeforeGain * noiseVarGain + kReadNoiseVarAfterGain;
            // Noise profile is the same across all 4 CFA channels
            double noiseProfile[2 * 4] = {
                noiseVarGain, readNoiseVar,
                noiseVarGain, readNoiseVar,
                noiseVarGain, readNoiseVar,
                noiseVarGain, readNoiseVar};
            nextResult->result_metadata->Set(ANDROID_SENSOR_NOISE_PROFILE, noiseProfile,
                    ARRAY_SIZE(noiseProfile));
        }

        auto b = nextBuffers->begin();
        while (b != nextBuffers->end()) {
            (*b)->streamBuffer.status = BufferStatus::kOk;
            switch ((*b)->format) {
                case HAL_PIXEL_FORMAT_RAW16:
                    if (!reprocessRequest) {
                        captureRaw((*b)->plane.img.img, settings.gain, (*b)->width);
                    } else {
                        ALOGE("%s: Reprocess requests with output format %x no supported!",
                                __FUNCTION__, (*b)->format);
                        (*b)->streamBuffer.status = BufferStatus::kError;
                    }
                    break;
                case HAL_PIXEL_FORMAT_RGB_888:
                    if (!reprocessRequest) {
                        captureRGB((*b)->plane.img.img, (*b)->width, (*b)->height,
                                (*b)->plane.img.stride, RGBLayout::RGB, settings.gain);
                    } else {
                        ALOGE("%s: Reprocess requests with output format %x no supported!",
                                __FUNCTION__, (*b)->format);
                        (*b)->streamBuffer.status = BufferStatus::kError;
                    }
                    break;
                case HAL_PIXEL_FORMAT_RGBA_8888:
                    if (!reprocessRequest) {
                        captureRGB((*b)->plane.img.img, (*b)->width, (*b)->height,
                                (*b)->plane.img.stride, RGBLayout::RGBA, settings.gain);
                    } else {
                        ALOGE("%s: Reprocess requests with output format %x no supported!",
                                __FUNCTION__, (*b)->format);
                        (*b)->streamBuffer.status = BufferStatus::kError;
                    }
                    break;
                case HAL_PIXEL_FORMAT_BLOB:
                    if ((*b)->dataSpace == HAL_DATASPACE_V0_JFIF) {
                        YUV420Frame yuvInput {
                                .width = reprocessRequest ? (*nextInputBuffer->begin())->width : 0,
                                .height = reprocessRequest ? (*nextInputBuffer->begin())->height :
                                        0,
                                .planes = reprocessRequest ?
                                        (*nextInputBuffer->begin())->plane.imgYCrCb : YCbCrPlanes{}
                        };
                        auto jpegInput = std::make_unique<JpegYUV420Input>();
                        jpegInput->width = (*b)->width;
                        jpegInput->height = (*b)->height;
                        auto img = new uint8_t[(jpegInput->width * jpegInput->height * 3) / 2];
                        jpegInput->yuvPlanes = {.imgY = img,
                            .imgCb = img + jpegInput->width * jpegInput->height,
                            .imgCr = img + (jpegInput->width * jpegInput->height * 5) / 4,
                            .yStride = jpegInput->width, .CbCrStride = jpegInput->width / 2,
                            .CbCrStep = 1 };
                        jpegInput->bufferOwner = true;
                        YUV420Frame yuvOutput {
                                .width = jpegInput->width,
                                .height = jpegInput->height,
                                .planes = jpegInput->yuvPlanes };

                        auto ret = processYUV420(yuvInput, yuvOutput, settings.gain,
                                reprocessRequest);
                        if (ret != 0) {
                            (*b)->streamBuffer.status = BufferStatus::kError;
                            break;
                        }

                        auto jpegJob = std::make_unique<JpegYUV420Job>();
                        jpegJob->input = std::move(jpegInput);
                        // If jpeg compression is successful, then the jpeg compressor
                        // must set the corresponding status.
                        (*b)->streamBuffer.status = BufferStatus::kError;
                        std::swap(jpegJob->output, *b);
                        jpegJob->resultMetadata = HalCameraMetadata::Clone(
                                nextResult->result_metadata.get());

                        Mutex::Autolock lock(mControlMutex);
                        mJpegCompressor->queueYUV420(std::move(jpegJob));
                    } else {
                        ALOGE("%s: Format %x with dataspace %x is TODO", __FUNCTION__, (*b)->format,
                                (*b)->dataSpace);
                        (*b)->streamBuffer.status = BufferStatus::kError;
                    }
                    break;
                case HAL_PIXEL_FORMAT_YCrCb_420_SP:
                case HAL_PIXEL_FORMAT_YCbCr_420_888:
                    {
                        YUV420Frame yuvInput {
                                .width = reprocessRequest ? (*nextInputBuffer->begin())->width : 0,
                                .height = reprocessRequest ? (*nextInputBuffer->begin())->height :
                                        0,
                                .planes = reprocessRequest ?
                                        (*nextInputBuffer->begin())->plane.imgYCrCb : YCbCrPlanes{}
                        };
                        YUV420Frame yuvOutput {
                                .width = (*b)->width,
                                .height = (*b)->height,
                                .planes = (*b)->plane.imgYCrCb };
                        auto ret = processYUV420(yuvInput, yuvOutput, settings.gain,
                                reprocessRequest);
                        if (ret != 0) {
                            (*b)->streamBuffer.status = BufferStatus::kError;
                        }
                    }
                    break;
                case HAL_PIXEL_FORMAT_Y16:
                    if (!reprocessRequest) {
                        if ((*b)->dataSpace == HAL_DATASPACE_DEPTH) {
                            captureDepth((*b)->plane.img.img, settings.gain, (*b)->width,
                                    (*b)->height, (*b)->plane.img.stride);
                        } else {
                            ALOGE("%s: Format %x with dataspace %x is TODO", __FUNCTION__,
                                    (*b)->format, (*b)->dataSpace);
                            (*b)->streamBuffer.status = BufferStatus::kError;
                        }
                    } else {
                        ALOGE("%s: Reprocess requests with output format %x no supported!",
                                __FUNCTION__, (*b)->format);
                        (*b)->streamBuffer.status = BufferStatus::kError;
                    }
                    break;
                default:
                    ALOGE("%s: Unknown format %x, no output", __FUNCTION__, (*b)->format);
                    (*b)->streamBuffer.status = BufferStatus::kError;
                    break;
            }

            b = nextBuffers->erase(b);
        }
    }

    if (reprocessRequest) {
        auto inputBuffer = nextInputBuffer->begin();
        while (inputBuffer != nextInputBuffer->end()) {
            (*inputBuffer++)->streamBuffer.status = BufferStatus::kOk;
        }
        nextInputBuffer->clear();
    }

    if ((callback.process_pipeline_result != nullptr) && (nextResult.get() != nullptr) &&
            (nextResult->result_metadata.get() != nullptr)) {
        callback.process_pipeline_result(std::move(nextResult));
    }

    ALOGVV("Sensor vertical blanking interval");
    nsecs_t workDoneRealTime = systemTime();
    const nsecs_t timeAccuracy = 2e6;  // 2 ms of imprecision is ok
    if (workDoneRealTime < frameEndRealTime - timeAccuracy) {
        timespec t;
        t.tv_sec = (frameEndRealTime - workDoneRealTime) / 1000000000L;
        t.tv_nsec = (frameEndRealTime - workDoneRealTime) % 1000000000L;

        int ret;
        do {
            ret = nanosleep(&t, &t);
        } while (ret != 0);
    }
    nsecs_t endRealTime __unused = systemTime();
    ALOGVV("Frame cycle took %" PRIu64 "  ms, target %" PRIu64 " ms",
            ns2ms(endRealTime - startRealTime), ns2ms(settings.frameDuration));

    return true;
};

void EmulatedSensor::captureRaw(uint8_t *img, uint32_t gain, uint32_t width) {
    ATRACE_CALL();
    float totalGain = gain / 100.0 * kBaseGainFactor;
    float noiseVarGain = totalGain * totalGain;
    float readNoiseVar =
        kReadNoiseVarBeforeGain * noiseVarGain + kReadNoiseVarAfterGain;
    //
    // RGGB
    int bayerSelect[4] = {EmulatedScene::R, EmulatedScene::Gr, EmulatedScene::Gb, EmulatedScene::B};
    mScene->setReadoutPixel(0, 0);
    for (unsigned int y = 0; y < mChars.height; y++) {
        int *bayerRow = bayerSelect + (y & 0x1) * 2;
        uint16_t *px = (uint16_t *)img + y * width;
        for (unsigned int x = 0; x < mChars.width; x++) {
            uint32_t electronCount;
            electronCount = mScene->getPixelElectrons()[bayerRow[x & 0x1]];

            // TODO: Better pixel saturation curve?
            electronCount = (electronCount < kSaturationElectrons)
                ? electronCount
                : kSaturationElectrons;

            // TODO: Better A/D saturation curve?
            uint16_t rawCount = electronCount * totalGain;
            rawCount = (rawCount < mChars.maxRawValue) ? rawCount : mChars.maxRawValue;

            // Calculate noise value
            // TODO: Use more-correct Gaussian instead of uniform noise
            float photonNoiseVar = electronCount * noiseVarGain;
            float noiseStddev = sqrtf_approx(readNoiseVar + photonNoiseVar);
            // Scaled to roughly match gaussian/uniform noise stddev
            float noiseSample = std::rand() * (2.5 / (1.0 + RAND_MAX)) - 1.25;

            rawCount += mChars.blackLevelPattern[bayerRow[x & 0x1]];
            rawCount += noiseStddev * noiseSample;

            *px++ = rawCount;
        }
        // TODO: Handle this better
        // simulatedTime += mRowReadoutTime;
    }
    ALOGVV("Raw sensor image captured");
}

void EmulatedSensor::captureRGB(uint8_t *img, uint32_t width, uint32_t height, uint32_t stride,
        RGBLayout layout, uint32_t gain) {
    ATRACE_CALL();
    float totalGain = gain / 100.0 * kBaseGainFactor;
    // In fixed-point math, calculate total scaling from electrons to 8bpp
    int scale64x = 64 * totalGain * 255 / mChars.maxRawValue;
    uint32_t incH = ceil((float)mChars.width / width);
    uint32_t incV = ceil((float)mChars.height / height);

    for (unsigned int y = 0, outY = 0; y < mChars.height; y += incV, outY++) {
        mScene->setReadoutPixel(0, y);
        uint8_t *px = img + outY * stride;
        for (unsigned int x = 0; x < mChars.width; x += incH) {
            uint32_t rCount, gCount, bCount;
            // TODO: Perfect demosaicing is a cheat
            const uint32_t *pixel = mScene->getPixelElectrons();
            rCount = pixel[EmulatedScene::R] * scale64x;
            gCount = pixel[EmulatedScene::Gr] * scale64x;
            bCount = pixel[EmulatedScene::B] * scale64x;

            uint8_t r = rCount < 255 * 64 ? rCount / 64 : 255;
            uint8_t g = gCount < 255 * 64 ? gCount / 64 : 255;
            uint8_t b = bCount < 255 * 64 ? bCount / 64 : 255;
            switch (layout) {
                case RGB:
                    *px++ = r;
                    *px++ = g;
                    *px++ = b;
                    break;
                case RGBA:
                    *px++ = r;
                    *px++ = g;
                    *px++ = b;
                    *px++ = 255;
                    break;
                case ARGB:
                    *px++ = 255;
                    *px++ = r;
                    *px++ = g;
                    *px++ = b;
                    break;
                default:
                    ALOGE("%s: RGB layout: %d not supported", __FUNCTION__, layout);
                    return;
            }
            for (unsigned int j = 1; j < incH; j++) mScene->getPixelElectrons();
        }
    }
    ALOGVV("RGB sensor image captured");
}

void EmulatedSensor::captureYUV420(YCbCrPlanes yuvLayout, uint32_t width, uint32_t height,
        uint32_t gain) {
    ATRACE_CALL();
    float totalGain = gain / 100.0 * kBaseGainFactor;
    // Using fixed-point math with 6 bits of fractional precision.
    // In fixed-point math, calculate total scaling from electrons to 8bpp
    const int scale64x = kFixedBitPrecision * totalGain * 255 / mChars.maxRawValue;
    // Fixed-point coefficients for RGB-YUV transform
    // Based on JFIF RGB->YUV transform.
    // Cb/Cr offset scaled by 64x twice since they're applied post-multiply
    const int rgbToY[] = {19, 37, 7};
    const int rgbToCb[] = {-10, -21, 32, 524288};
    const int rgbToCr[] = {32, -26, -5, 524288};
    // Scale back to 8bpp non-fixed-point
    const int scaleOut = 64;
    const int scaleOutSq = scaleOut * scaleOut;  // after multiplies

    // inc = how many pixels to skip while reading every next pixel
    uint32_t incH = ceil((float)mChars.width / width);
    uint32_t incV = ceil((float)mChars.height / height);
    for (unsigned int y = 0, outY = 0; y < mChars.height; y += incV, outY++) {
        uint8_t *pxY = yuvLayout.imgY + outY * yuvLayout.yStride;
        uint8_t *pxCb = yuvLayout.imgCb + (outY / 2) * yuvLayout.CbCrStride;
        uint8_t *pxCr = yuvLayout.imgCr + (outY / 2) * yuvLayout.CbCrStride;
        mScene->setReadoutPixel(0, y);
        for (unsigned int outX = 0; outX < yuvLayout.yStride; outX++) {
            int32_t rCount, gCount, bCount;
            // TODO: Perfect demosaicing is a cheat
            const uint32_t *pixel = mScene->getPixelElectrons();
            rCount = pixel[EmulatedScene::R] * scale64x;
            rCount = rCount < kSaturationPoint ? rCount : kSaturationPoint;
            gCount = pixel[EmulatedScene::Gr] * scale64x;
            gCount = gCount < kSaturationPoint ? gCount : kSaturationPoint;
            bCount = pixel[EmulatedScene::B] * scale64x;
            bCount = bCount < kSaturationPoint ? bCount : kSaturationPoint;

            // Gamma correction
            rCount = mGammaTable[rCount];
            gCount = mGammaTable[gCount];
            bCount = mGammaTable[bCount];

            *pxY++ = (rgbToY[0] * rCount + rgbToY[1] * gCount + rgbToY[2] * bCount) / scaleOutSq;
            if (outY % 2 == 0 && outX % 2 == 0) {
                *pxCb = (rgbToCb[0] * rCount + rgbToCb[1] * gCount +
                        rgbToCb[2] * bCount + rgbToCb[3]) /
                    scaleOutSq;
                *pxCr = (rgbToCr[0] * rCount + rgbToCr[1] * gCount +
                        rgbToCr[2] * bCount + rgbToCr[3]) /
                    scaleOutSq;
                pxCr += yuvLayout.CbCrStep; pxCb += yuvLayout.CbCrStep;
            }

            // Skip unprocessed pixels from sensor.
            for (unsigned int j = 1; j < incH; j++) mScene->getPixelElectrons();
        }
    }
    ALOGVV("YUV420 sensor image captured");
}

void EmulatedSensor::captureDepth(uint8_t *img, uint32_t gain, uint32_t width, uint32_t height,
        uint32_t stride) {
    ATRACE_CALL();
    float totalGain = gain / 100.0 * kBaseGainFactor;
    // In fixed-point math, calculate scaling factor to 13bpp millimeters
    int scale64x = 64 * totalGain * 8191 / mChars.maxRawValue;
    uint32_t incH = ceil((float)mChars.width / width);
    uint32_t incV = ceil((float)mChars.height / height);

    for (unsigned int y = 0, outY = 0; y < mChars.height; y += incV, outY++) {
        mScene->setReadoutPixel(0, y);
        uint16_t *px = ((uint16_t *)img) + outY * stride;
        for (unsigned int x = 0; x < mChars.width; x += incH) {
            uint32_t depthCount;
            // TODO: Make up real depth scene instead of using green channel
            // as depth
            const uint32_t *pixel = mScene->getPixelElectrons();
            depthCount = pixel[EmulatedScene::Gr] * scale64x;

            *px++ = depthCount < 8191 * 64 ? depthCount / 64 : 0;
            for (unsigned int j = 1; j < incH; j++) mScene->getPixelElectrons();
        }
        // TODO: Handle this better
        // simulatedTime += mRowReadoutTime;
    }
    ALOGVV("Depth sensor image captured");
}

status_t EmulatedSensor::processYUV420(const YUV420Frame& input, const YUV420Frame& output,
        uint32_t gain, bool reprocessRequest) {
    ATRACE_CALL();
    size_t inputWidth, inputHeight;
    YCbCrPlanes yuvPlanes;
    std::vector<uint8_t> tempYUV;
    if (reprocessRequest) {
        inputWidth = input.width;
        inputHeight = input.height;
        yuvPlanes = input.planes;
    } else {
        // Generate the smallest possible frame with the expected AR and
        // then scale using libyuv.
        float aspectRatio = static_cast<float>(output.width) / output.height;
        inputWidth = EmulatedScene::kSceneWidth * aspectRatio;
        inputHeight = EmulatedScene::kSceneHeight;
        tempYUV.reserve((inputWidth * inputHeight * 3) / 2);
        auto tempYUVBuffer = tempYUV.data();
        yuvPlanes = {.imgY = tempYUVBuffer,
            .imgCb = tempYUVBuffer + inputWidth * inputHeight,
            .imgCr = tempYUVBuffer + (inputWidth * inputHeight * 5) / 4,
            .yStride = inputWidth, .CbCrStride = inputWidth / 2,
            .CbCrStep = 1 };
        captureYUV420(yuvPlanes, inputWidth, inputHeight, gain);
    }

    auto ret = I420Scale(
            yuvPlanes.imgY,
            yuvPlanes.yStride,
            yuvPlanes.imgCb,
            yuvPlanes.CbCrStride,
            yuvPlanes.imgCr,
            yuvPlanes.CbCrStride,
            inputWidth,
            inputHeight,
            output.planes.imgY,
            output.planes.yStride,
            output.planes.imgCb,
            output.planes.CbCrStride,
            output.planes.imgCr,
            output.planes.CbCrStride,
            output.width,
            output.height,
            libyuv::kFilterNone);
    if (ret != 0) {
        ALOGE("%s: Failed during YUV scaling: %d", __FUNCTION__, ret);
    }

    return ret;
}

int32_t EmulatedSensor::applysRGBGamma(int32_t value, int32_t saturation) {
    float nValue = (static_cast<float>(value) / saturation);
    nValue = (nValue <= 0.0031308f) ? nValue * 12.92f : 1.055f * pow(nValue, 0.4166667f) - 0.055f;
    return nValue * saturation;
}

}  // namespace android
