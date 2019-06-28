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

#ifdef LOG_NNDEBUG
#define ALOGVV(...) ALOGV(__VA_ARGS__)
#else
#define ALOGVV(...) ((void)0)
#endif

#include <utils/Log.h>

#include <cmath>
#include <cstdlib>
#include "EmulatedSensor.h"
#include <inttypes.h>
#include "system/camera_metadata.h"

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
const uint32_t EmulatedSensor::kMaxStallingStreams = 1;

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

EmulatedSensor::EmulatedSensor() : Thread(false),
        mGotVSync(false) {
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

    return true;
}

bool EmulatedSensor::isStreamCombinationSupported(const StreamConfiguration& config,
        StreamConfigurationMap& map, const SensorCharacteristics& sensorChars) {
    uint32_t rawStreamCount = 0;
    uint32_t processedStreamCount = 0;
    uint32_t stallingStreamCount = 0;

    for (const auto& stream : config.streams) {
        if (stream.rotation != google_camera_hal::StreamRotation::kRotation0) {
            ALOGE("%s: Stream rotation: 0x%x not supported!", __FUNCTION__, stream.rotation);
            return false;
        }

        if (stream.stream_type != google_camera_hal::StreamType::kOutput) {
            ALOGE("%s: Stream type: 0x%x not supported!", __FUNCTION__, stream.stream_type);
            return false;
        }

        auto format = overrideFormat(stream.format);
        switch (format) {
            case HAL_PIXEL_FORMAT_BLOB:
                if (stream.data_space != HAL_DATASPACE_V0_JFIF) {
                    ALOGE("%s: Unsupported Blob dataspace 0x%x", __FUNCTION__, stream.data_space);
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

        auto outputSizes = map.getOutputSizes(format);
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

    return true;
}

status_t EmulatedSensor::startUp(SensorCharacteristics characteristics) {
    ALOGV("%s: E", __FUNCTION__);

    if (!areCharacteristicsSupported(characteristics)) {
        ALOGE("%s: Sensor characteristics not supported!", __FUNCTION__);
        return BAD_VALUE;
    }

    if (isRunning()) {
        return OK;
    }

    mChars = characteristics;
    mScene = std::make_unique<EmulatedScene>(mChars.width, mChars.height, kElectronsPerLuxSecond);
    mRowReadoutTime = mChars.frameDurationRange[0] / mChars.height;
    kBaseGainFactor = (float)mChars.maxRawValue / EmulatedSensor::kSaturationElectrons;
    mJpegCompressor = std::make_unique<JpegCompressor>();

    int res;
    res = run(LOG_TAG, ANDROID_PRIORITY_URGENT_DISPLAY);
    if (res != OK) {
        ALOGE("Unable to start up sensor capture thread: %d", res);
    }
    return res;
}

status_t EmulatedSensor::shutDown() {
    ALOGV("%s: E", __FUNCTION__);

    int res;
    res = requestExitAndWait();
    if (res != OK) {
        ALOGE("Unable to shut down sensor capture thread: %d", res);
    }
    mJpegCompressor.release();
    return res;
}

void EmulatedSensor::setCurrentRequest(SensorSettings settings,
        std::unique_ptr<HwlPipelineResult> result, std::unique_ptr<Buffers> outputBuffers) {
    Mutex::Autolock lock(mControlMutex);
    mCurrentSettings = settings;
    mCurrentResult = std::move(result);
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
    mJpegCompressor = std::make_unique<JpegCompressor>();

    // Then return any pending frames here
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
    /**
     * Sensor capture operation main loop.
     *
     */

    /**
     * Stage 1: Read in latest control parameters
     */
    std::unique_ptr<Buffers> nextBuffers;
    std::unique_ptr<HwlPipelineResult> nextResult;
    SensorSettings settings;
    {
        Mutex::Autolock lock(mControlMutex);
        settings = mCurrentSettings;
        std::swap(nextBuffers, mCurrentOutputBuffers);
        std::swap(nextResult, mCurrentResult);

        // Signal VSync for start of readout
        ALOGVV("Sensor VSync");
        mGotVSync = true;
        mVSync.signal();
    }

    nsecs_t startRealTime = systemTime();
    // Stagefright cares about system time for timestamps, so base simulated
    // time on that.
    nsecs_t frameEndRealTime = startRealTime + settings.frameDuration;

    /**
     * Stage 2: Capture new image
     */
    mNextCaptureTime = frameEndRealTime;

    if (nextBuffers != nullptr) {
        if (nextBuffers->at(0)->callback.notify != nullptr) {
            NotifyMessage msg {
                .type = MessageType::kShutter,
                .message.shutter = {
                    .frame_number = nextBuffers->at(0)->frameNumber,
                    .timestamp_ns = static_cast<uint64_t>(mNextCaptureTime)
                }
            };
            nextBuffers->at(0)->callback.notify(nextResult->pipeline_id, msg);
        }
        ALOGVV("Starting next capture: Exposure: %f ms, gain: %d", ns2ms(settings.exposureTime),
                gain);
        mScene->setExposureDuration((float)settings.exposureTime / 1e9);
        mScene->calculateScene(mNextCaptureTime);
        nextResult->result_metadata->Set(ANDROID_SENSOR_TIMESTAMP, &mNextCaptureTime, 1);

        auto b = nextBuffers->begin();
        while (b != nextBuffers->end()) {
            (*b)->streamBuffer.status = BufferStatus::kOk;
            switch ((*b)->format) {
                case HAL_PIXEL_FORMAT_RAW16:
                    captureRaw((*b)->plane.img.img, settings.gain, (*b)->width);
                    break;
                case HAL_PIXEL_FORMAT_RGB_888:
                    captureRGB((*b)->plane.img.img, settings.gain, (*b)->width,
                            (*b)->plane.img.stride);
                    break;
                case HAL_PIXEL_FORMAT_RGBA_8888:
                    captureRGBA((*b)->plane.img.img, settings.gain, (*b)->width,
                            (*b)->plane.img.stride);
                    break;
                case HAL_PIXEL_FORMAT_BLOB:
                    if ((*b)->dataSpace == HAL_DATASPACE_V0_JFIF) {
                        // Add auxillary buffer of the right size
                        // Assumes only one BLOB (JPEG) buffer in
                        // mNextCapturedBuffers
                        auto jpegInput = std::make_unique<JpegRGBInput>();
                        jpegInput->width = (*b)->width;
                        jpegInput->height = (*b)->height;
                        jpegInput->stride = (*b)->width * 3;
                        jpegInput->img = new uint8_t[jpegInput->stride * (*b)->height];
                        captureRGB(jpegInput->img, settings.gain, (*b)->width, jpegInput->stride);

                        auto jpegResult = std::make_unique<HwlPipelineResult>();
                        jpegResult->camera_id = nextResult->camera_id;
                        jpegResult->pipeline_id = nextResult->pipeline_id;
                        jpegResult->frame_number = nextResult->frame_number;
                        jpegResult->result_metadata = HalCameraMetadata::Clone(
                                nextResult->result_metadata.get());

                        auto jpegJob = std::make_unique<JpegJob>();
                        jpegJob->input = std::move(jpegInput);
                        // If jpeg compression is successful, then the jpeg compressor
                        // must set the corresponding status.
                        (*b)->streamBuffer.status = BufferStatus::kError;
                        std::swap(jpegJob->output, *b);
                        jpegJob->result = std::move(jpegResult);

                        Mutex::Autolock lock(mControlMutex);
                        auto ret = mJpegCompressor->queue(std::move(jpegJob));
                        if (ret == OK) {
                            nextResult->partial_result = 0;
                            nextResult->result_metadata.release();
                        }
                    } else {
                        ALOGE("%s: Format %x with dataspace %x is TODO", __FUNCTION__, (*b)->format,
                                (*b)->dataSpace);
                        (*b)->streamBuffer.status = BufferStatus::kError;
                    }
                    break;
                case HAL_PIXEL_FORMAT_YCrCb_420_SP:
                case HAL_PIXEL_FORMAT_YCbCr_420_888:
                    captureNV21((*b)->plane.imgYCrCb, settings.gain, (*b)->width);
                    break;
                case HAL_PIXEL_FORMAT_Y16:
                    captureDepth((*b)->plane.img.img, settings.gain, (*b)->width,
                            (*b)->plane.img.stride);
                    break;
                default:
                    ALOGE("%s: Unknown format %x, no output", __FUNCTION__, (*b)->format);
                    (*b)->streamBuffer.status = BufferStatus::kError;
                    break;
            }

            if ((*b).get() == nullptr) {
                b = nextBuffers->erase(b);
            } else {
                b++;
            }
        }
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

    if ((nextBuffers.get() != nullptr) && !nextBuffers->empty() &&
            (nextResult.get() != nullptr) && (nextResult->result_metadata.get() != nullptr)) {
        nextBuffers->at(0)->callback.process_pipeline_result(std::move(nextResult));
    }

    return true;
};

void EmulatedSensor::captureRaw(uint8_t *img, uint32_t gain, uint32_t width) {
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

void EmulatedSensor::captureRGBA(uint8_t *img, uint32_t gain, uint32_t width,  uint32_t stride) {
    float totalGain = gain / 100.0 * kBaseGainFactor;
    // In fixed-point math, calculate total scaling from electrons to 8bpp
    int scale64x = 64 * totalGain * 255 / mChars.maxRawValue;
    uint32_t inc = ceil((float)mChars.width / width);

    for (unsigned int y = 0, outY = 0; y < mChars.height; y += inc, outY++) {
        uint8_t *px = img + outY * stride;
        mScene->setReadoutPixel(0, y);
        for (unsigned int x = 0; x < mChars.width; x += inc) {
            uint32_t rCount, gCount, bCount;
            // TODO: Perfect demosaicing is a cheat
            const uint32_t *pixel = mScene->getPixelElectrons();
            rCount = pixel[EmulatedScene::R] * scale64x;
            gCount = pixel[EmulatedScene::Gr] * scale64x;
            bCount = pixel[EmulatedScene::B] * scale64x;

            *px++ = rCount < 255 * 64 ? rCount / 64 : 255;
            *px++ = gCount < 255 * 64 ? gCount / 64 : 255;
            *px++ = bCount < 255 * 64 ? bCount / 64 : 255;
            *px++ = 255;
            for (unsigned int j = 1; j < inc; j++) mScene->getPixelElectrons();
        }
        // TODO: Handle this better
        // simulatedTime += mRowReadoutTime;
    }
    ALOGVV("RGBA sensor image captured");
}

void EmulatedSensor::captureRGB(uint8_t *img, uint32_t gain, uint32_t width, uint32_t stride) {
    float totalGain = gain / 100.0 * kBaseGainFactor;
    // In fixed-point math, calculate total scaling from electrons to 8bpp
    int scale64x = 64 * totalGain * 255 / mChars.maxRawValue;
    uint32_t inc = ceil((float)mChars.width / width);

    for (unsigned int y = 0, outY = 0; y < mChars.height; y += inc, outY++) {
        mScene->setReadoutPixel(0, y);
        uint8_t *px = img + outY * stride;
        for (unsigned int x = 0; x < mChars.width; x += inc) {
            uint32_t rCount, gCount, bCount;
            // TODO: Perfect demosaicing is a cheat
            const uint32_t *pixel = mScene->getPixelElectrons();
            rCount = pixel[EmulatedScene::R] * scale64x;
            gCount = pixel[EmulatedScene::Gr] * scale64x;
            bCount = pixel[EmulatedScene::B] * scale64x;

            *px++ = rCount < 255 * 64 ? rCount / 64 : 255;
            *px++ = gCount < 255 * 64 ? gCount / 64 : 255;
            *px++ = bCount < 255 * 64 ? bCount / 64 : 255;
            for (unsigned int j = 1; j < inc; j++) mScene->getPixelElectrons();
        }
        // TODO: Handle this better
        // simulatedTime += mRowReadoutTime;
    }
    ALOGVV("RGB sensor image captured");
}

void EmulatedSensor::captureNV21(YCbCrPlanes yuvLayout, uint32_t gain, uint32_t width) {
    float totalGain = gain / 100.0 * kBaseGainFactor;
    // Using fixed-point math with 6 bits of fractional precision.
    // In fixed-point math, calculate total scaling from electrons to 8bpp
    const int scale64x = 64 * totalGain * 255 / mChars.maxRawValue;
    // In fixed-point math, saturation point of sensor after gain
    const int saturationPoint = 64 * 255;
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
    // horizontally.
    uint32_t inc = ceil((float)mChars.width / width);
    for (unsigned int y = 0, outY = 0; y < mChars.height; y += inc, outY++) {
        uint8_t *pxY = yuvLayout.imgY + outY * yuvLayout.yStride;
        uint8_t *pxCb = yuvLayout.imgCb + (outY / 2) * yuvLayout.CbCrStride;
        uint8_t *pxCr = yuvLayout.imgCr + (outY / 2) * yuvLayout.CbCrStride;
        mScene->setReadoutPixel(0, y);
        for (unsigned int outX = 0; outX < yuvLayout.yStride; outX++) {
            int32_t rCount, gCount, bCount;
            // TODO: Perfect demosaicing is a cheat
            const uint32_t *pixel = mScene->getPixelElectrons();
            rCount = pixel[EmulatedScene::R] * scale64x;
            rCount = rCount < saturationPoint ? rCount : saturationPoint;
            gCount = pixel[EmulatedScene::Gr] * scale64x;
            gCount = gCount < saturationPoint ? gCount : saturationPoint;
            bCount = pixel[EmulatedScene::B] * scale64x;
            bCount = bCount < saturationPoint ? bCount : saturationPoint;

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
            for (unsigned int j = 1; j < inc; j++) mScene->getPixelElectrons();
        }
    }
    ALOGVV("NV21 sensor image captured");
}

void EmulatedSensor::captureDepth(uint8_t *img, uint32_t gain, uint32_t width, uint32_t stride) {
    float totalGain = gain / 100.0 * kBaseGainFactor;
    // In fixed-point math, calculate scaling factor to 13bpp millimeters
    int scale64x = 64 * totalGain * 8191 / mChars.maxRawValue;
    uint32_t inc = ceil((float)mChars.width / width);

    for (unsigned int y = 0, outY = 0; y < mChars.height; y += inc, outY++) {
        mScene->setReadoutPixel(0, y);
        uint16_t *px = ((uint16_t *)img) + outY * stride;
        for (unsigned int x = 0; x < mChars.width; x += inc) {
            uint32_t depthCount;
            // TODO: Make up real depth scene instead of using green channel
            // as depth
            const uint32_t *pixel = mScene->getPixelElectrons();
            depthCount = pixel[EmulatedScene::Gr] * scale64x;

            *px++ = depthCount < 8191 * 64 ? depthCount / 64 : 0;
            for (unsigned int j = 1; j < inc; j++) mScene->getPixelElectrons();
        }
        // TODO: Handle this better
        // simulatedTime += mRowReadoutTime;
    }
    ALOGVV("Depth sensor image captured");
}

}  // namespace android
