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

//#define LOG_NDEBUG 0
#define LOG_TAG "EmulatedTorchState"
#include <log/log.h>

#include "EmulatedTorchState.h"

namespace android {

using android::google_camera_hal::TorchModeStatus;

status_t EmulatedTorchState::setTorchMode(TorchMode mode) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mCameraOpen) {
        ALOGE("%s: Camera device open, torch cannot be controlled using this API!",
                __FUNCTION__);
        return UNKNOWN_ERROR;
    }

    mTorchCb(mCameraId, (mode == TorchMode::kOn) ? TorchModeStatus::kAvailableOn :
            TorchModeStatus::kAvailableOff);

    return OK;
}

void EmulatedTorchState::acquireFlashHw() {
    std::lock_guard<std::mutex> lock(mMutex);
    mCameraOpen = true;
    mTorchCb(mCameraId, TorchModeStatus::kNotAvailable);
}

void EmulatedTorchState::releaseFlashHw() {
    std::lock_guard<std::mutex> lock(mMutex);
    mCameraOpen = false;
    mTorchCb(mCameraId, TorchModeStatus::kAvailableOff);
}

}  // namespace android
