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

#ifndef HW_EMULATOR_TORCH_STATE_H
#define HW_EMULATOR_TORCH_STATE_H

#include <hwl_types.h>
#include <mutex>
#include <queue>

namespace android {

using android::google_camera_hal::TorchMode;
using android::google_camera_hal::HwlTorchModeStatusChangeFunc;

class EmulatedTorchState {
public:
    EmulatedTorchState(uint32_t cameraId, HwlTorchModeStatusChangeFunc torchCb) :
            mCameraId(cameraId), mTorchCb(torchCb) {}

    status_t setTorchMode(TorchMode mode);
    void acquireFlashHw();
    void releaseFlashHw();

private:
    std::mutex mMutex;

    uint32_t mCameraId;
    HwlTorchModeStatusChangeFunc mTorchCb;
    bool mCameraOpen = false;

    EmulatedTorchState(const EmulatedTorchState&) = delete;
    EmulatedTorchState& operator = (const EmulatedTorchState&) = delete;
};

}  // namespace android

#endif
