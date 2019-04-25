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
#include "system/camera_metadata.h"

namespace android {

 // 1 us - 30 sec
const nsecs_t EmulatedSensor::kSupportedExposureTimeRange[2] = {1000LL, 30000000000LL} ;

 // ~1/30 s - 30 sec
const nsecs_t EmulatedSensor::kSupportedFrameDurationRange[2] = {33331760LL, 30000000000LL};

const int32_t EmulatedSensor::kSupportedSensitivityRange[2] = {100, 1600};
const uint32_t EmulatedSensor::kDefaultSensitivity = 100;

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
        (EmulatedSensor::kSaturationVoltage * EmulatedSensor::kVoltsPerLuxSecond);

const float EmulatedSensor::kReadNoiseStddevBeforeGain = 1.177;  // in electrons
const float EmulatedSensor::kReadNoiseStddevAfterGain = 2.100;   // in digital counts
const float EmulatedSensor::kReadNoiseVarBeforeGain =
        EmulatedSensor::kReadNoiseStddevBeforeGain * EmulatedSensor::kReadNoiseStddevBeforeGain;
const float EmulatedSensor::kReadNoiseVarAfterGain =
        EmulatedSensor::kReadNoiseStddevAfterGain * EmulatedSensor::kReadNoiseStddevAfterGain;


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
        mGotVSync(false),
        mCapturedBuffers(nullptr), mListener(nullptr) {
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

    int res;
    mCapturedBuffers = nullptr;
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
    return res;
}

void EmulatedSensor::setCurrentSettings(SensorSettings settings) {
    Mutex::Autolock lock(mControlMutex);
    mCurrentSettings = settings;
}

bool EmulatedSensor::waitForVSync(nsecs_t reltime) {
    int res;
    Mutex::Autolock lock(mControlMutex);

    mGotVSync = false;
    res = mVSync.waitRelative(mControlMutex, reltime);
    if (res != OK && res != TIMED_OUT) {
        ALOGE("%s: Error waiting for VSync signal: %d", __FUNCTION__, res);
        return false;
    }
    return mGotVSync;
}

bool EmulatedSensor::waitForNewFrame(nsecs_t reltime, nsecs_t *captureTime) {
    Mutex::Autolock lock(mReadoutMutex);

    if (mCapturedBuffers == NULL) {
        int res;
        res = mReadoutAvailable.waitRelative(mReadoutMutex, reltime);
        if (res == TIMED_OUT) {
            return false;
        } else if (res != OK || mCapturedBuffers == NULL) {
            ALOGE("Error waiting for sensor readout signal: %d", res);
            return false;
        }
    }
    mReadoutComplete.signal();

    *captureTime = mCaptureTime;
    mCapturedBuffers = NULL;
    return true;
}

EmulatedSensor::SensorListener::~SensorListener() {}

void EmulatedSensor::setSensorListener(SensorListener *listener) {
    Mutex::Autolock lock(mControlMutex);
    mListener = listener;
}

status_t EmulatedSensor::readyToRun() {
    ALOGV("Starting up sensor thread");
    mStartupTime = systemTime();
    mNextCaptureTime = 0;
    mNextCapturedBuffers = NULL;
    return OK;
}

bool EmulatedSensor::threadLoop() {
    /**
     * Sensor capture operation main loop.
     *
     * Stages are out-of-order relative to a single frame's processing, but
     * in-order in time.
     */

    /**
     * Stage 1: Read in latest control parameters
     */
    uint64_t exposureDuration;
    uint64_t frameDuration;
    uint32_t gain;
    Buffers *nextBuffers;
    uint32_t frameNumber;
    SensorListener *listener = NULL;
    {
        Mutex::Autolock lock(mControlMutex);
        exposureDuration = mCurrentSettings.exposureTime;
        frameDuration = mCurrentSettings.frameDuration;
        gain = mCurrentSettings.gain;
        nextBuffers = mCurrentSettings.outputBuffers;
        frameNumber = mCurrentSettings.frameNumber;
        listener = mListener;
        // Don't reuse a buffer set
        mCurrentSettings.outputBuffers = nullptr;

        // Signal VSync for start of readout
        ALOGVV("Sensor VSync");
        mGotVSync = true;
        mVSync.signal();
    }

    /**
     * Stage 3: Read out latest captured image
     */

    Buffers *capturedBuffers = NULL;
    nsecs_t captureTime = 0;

    nsecs_t startRealTime = systemTime();
    // Stagefright cares about system time for timestamps, so base simulated
    // time on that.
    nsecs_t simulatedTime = startRealTime;
    nsecs_t frameEndRealTime = startRealTime + frameDuration;

    if (mNextCapturedBuffers != NULL) {
        ALOGVV("Sensor starting readout");
        // Pretend we're doing readout now; will signal once enough time has elapsed
        capturedBuffers = mNextCapturedBuffers;
        captureTime = mNextCaptureTime;
    }
    simulatedTime += mRowReadoutTime + kMinVerticalBlank;

    // TODO: Move this signal to another thread to simulate readout
    // time properly
    if (capturedBuffers != NULL) {
        ALOGVV("Sensor readout complete");
        Mutex::Autolock lock(mReadoutMutex);
        if (mCapturedBuffers != NULL) {
            ALOGV("Waiting for readout thread to catch up!");
            mReadoutComplete.wait(mReadoutMutex);
        }

        mCapturedBuffers = capturedBuffers;
        mCaptureTime = captureTime;
        mReadoutAvailable.signal();
        capturedBuffers = NULL;
    }

    /**
     * Stage 2: Capture new image
     */
    mNextCaptureTime = simulatedTime;
    mNextCapturedBuffers = nextBuffers;

    if (mNextCapturedBuffers != NULL) {
        if (listener != NULL) {
            listener->onSensorEvent(frameNumber, SensorListener::EXPOSURE_START,
                    mNextCaptureTime);
        }
        ALOGVV("Starting next capture: Exposure: %f ms, gain: %d",
                (float)exposureDuration / 1e6, gain);
        mScene->setExposureDuration((float)exposureDuration / 1e9);
        mScene->calculateScene(mNextCaptureTime);

        // Might be adding more buffers, so size isn't constant
        for (size_t i = 0; i < mNextCapturedBuffers->size(); i++) {
            const StreamBuffer &b = (*mNextCapturedBuffers)[i];
            ALOGVV(
                    "Sensor capturing buffer %d: stream %d,"
                    " %d x %d, format %x, stride %d, buf %p, img %p",
                    i, b.streamId, b.width, b.height, b.format, b.stride, b.buffer,
                    b.img);
            switch (b.format) {
                case HAL_PIXEL_FORMAT_RAW16:
                    captureRaw(b.img, gain, b.stride);
                    break;
                case HAL_PIXEL_FORMAT_RGB_888:
                    captureRGB(b.img, gain, b.stride);
                    break;
                case HAL_PIXEL_FORMAT_RGBA_8888:
                    captureRGBA(b.img, gain, b.stride);
                    break;
                case HAL_PIXEL_FORMAT_BLOB:
                    if (b.dataSpace != HAL_DATASPACE_DEPTH) {
                        // Add auxillary buffer of the right size
                        // Assumes only one BLOB (JPEG) buffer in
                        // mNextCapturedBuffers
                        StreamBuffer bAux;
                        bAux.streamId = 0;
                        bAux.width = b.width;
                        bAux.height = b.height;
                        bAux.format = HAL_PIXEL_FORMAT_RGB_888;
                        bAux.stride = b.width;
                        bAux.buffer = NULL;
                        // TODO: Reuse these
                        bAux.img = new uint8_t[b.width * b.height * 3];
                        mNextCapturedBuffers->push_back(bAux);
                    } else {
                        ALOGE("%s: Format %x with dataspace %x is TODO", __FUNCTION__, b.format, b.dataSpace);
                    }
                    break;
                case HAL_PIXEL_FORMAT_YCrCb_420_SP:
                case HAL_PIXEL_FORMAT_YCbCr_420_888:
                    captureNV21(b.img, gain, b.stride);
                    break;
                case HAL_PIXEL_FORMAT_Y16:
                    captureDepth(b.img, gain, b.stride);
                    break;
                default:
                    ALOGE("%s: Unknown format %x, no output", __FUNCTION__, b.format);
                    break;
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
    ALOGVV("Frame cycle took %d ms, target %d ms",
            (int)((endRealTime - startRealTime) / 1000000),
            (int)(frameDuration / 1000000));
    return true;
};

void EmulatedSensor::captureRaw(uint8_t *img, uint32_t gain, uint32_t stride) {
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
        uint16_t *px = (uint16_t *)img + y * stride;
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

void EmulatedSensor::captureRGBA(uint8_t *img, uint32_t gain, uint32_t stride) {
    float totalGain = gain / 100.0 * kBaseGainFactor;
    // In fixed-point math, calculate total scaling from electrons to 8bpp
    int scale64x = 64 * totalGain * 255 / mChars.maxRawValue;
    uint32_t inc = ceil((float)mChars.width / stride);

    for (unsigned int y = 0, outY = 0; y < mChars.height; y += inc, outY++) {
        uint8_t *px = img + outY * stride * 4;
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

void EmulatedSensor::captureRGB(uint8_t *img, uint32_t gain, uint32_t stride) {
    float totalGain = gain / 100.0 * kBaseGainFactor;
    // In fixed-point math, calculate total scaling from electrons to 8bpp
    int scale64x = 64 * totalGain * 255 / mChars.maxRawValue;
    uint32_t inc = ceil((float)mChars.width / stride);

    for (unsigned int y = 0, outY = 0; y < mChars.height; y += inc, outY++) {
        mScene->setReadoutPixel(0, y);
        uint8_t *px = img + outY * stride * 3;
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

void EmulatedSensor::captureNV21(uint8_t *img, uint32_t gain, uint32_t stride) {
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
    uint32_t inc = ceil((float)mChars.width / stride);
    // outH = projected vertical resolution based on stride.
    uint32_t outH = mChars.height / inc;
    for (unsigned int y = 0, outY = 0; y < mChars.height; y += inc, outY++) {
        uint8_t *pxY = img + outY * stride;
        uint8_t *pxVU = img + (outH + outY / 2) * stride;
        mScene->setReadoutPixel(0, y);
        for (unsigned int outX = 0; outX < stride; outX++) {
            int32_t rCount, gCount, bCount;
            // TODO: Perfect demosaicing is a cheat
            const uint32_t *pixel = mScene->getPixelElectrons();
            rCount = pixel[EmulatedScene::R] * scale64x;
            rCount = rCount < saturationPoint ? rCount : saturationPoint;
            gCount = pixel[EmulatedScene::Gr] * scale64x;
            gCount = gCount < saturationPoint ? gCount : saturationPoint;
            bCount = pixel[EmulatedScene::B] * scale64x;
            bCount = bCount < saturationPoint ? bCount : saturationPoint;

            *pxY++ = (rgbToY[0] * rCount + rgbToY[1] * gCount + rgbToY[2] * bCount) /
                scaleOutSq;
            if (outY % 2 == 0 && outX % 2 == 0) {
                *pxVU++ = (rgbToCb[0] * rCount + rgbToCb[1] * gCount +
                        rgbToCb[2] * bCount + rgbToCb[3]) /
                    scaleOutSq;
                *pxVU++ = (rgbToCr[0] * rCount + rgbToCr[1] * gCount +
                        rgbToCr[2] * bCount + rgbToCr[3]) /
                    scaleOutSq;
            }

            // Skip unprocessed pixels from sensor.
            for (unsigned int j = 1; j < inc; j++) mScene->getPixelElectrons();
        }
    }
    ALOGVV("NV21 sensor image captured");
}

void EmulatedSensor::captureDepth(uint8_t *img, uint32_t gain, uint32_t stride) {
    float totalGain = gain / 100.0 * kBaseGainFactor;
    // In fixed-point math, calculate scaling factor to 13bpp millimeters
    int scale64x = 64 * totalGain * 8191 / mChars.maxRawValue;
    uint32_t inc = ceil((float)mChars.width / stride);

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
