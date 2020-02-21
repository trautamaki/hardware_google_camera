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

#ifndef HARDWARE_GOOGLE_CAMERA_HAL_UTILS_UTILS_H_
#define HARDWARE_GOOGLE_CAMERA_HAL_UTILS_UTILS_H_

#include "hal_types.h"

namespace android {
namespace google_camera_hal {
namespace utils {

bool IsPreviewStream(const Stream& stream);
bool IsJPEGSnapshotStream(const Stream& stream);
bool IsVideoStream(const Stream& stream);
bool IsRawStream(const Stream& stream);
bool IsInputRawStream(const Stream& stream);
bool IsArbitraryDataSpaceRawStream(const Stream& stream);
bool IsYUVSnapshotStream(const Stream& stream);
bool IsDepthStream(const Stream& stream);
bool IsOutputZslStream(const Stream& stream);

status_t GetSensorPhysicalSize(const HalCameraMetadata* characteristics,
                               float* width, float* height);

status_t GetSensorActiveArraySize(const HalCameraMetadata* characteristics,
                                  Rect* active_array);

status_t GetSensorPixelArraySize(const HalCameraMetadata* characteristics,
                                 Dimension* pixel_array);

status_t GetFocalLength(const HalCameraMetadata* characteristics,
                        float* focal_length);

// Return if LiveSnapshot is configured
bool IsLiveSnapshotConfigured(const StreamConfiguration& stream_config);

// Return true if max fps is the same at high speed mode
bool IsHighSpeedModeFpsCompatible(StreamConfigurationMode mode,
                                  const HalCameraMetadata* old_session,
                                  const HalCameraMetadata* new_session);

// This method is for IsReconfigurationRequired purpose
// Return true if meet any condition below
// 1. Any session entry count is zero
// 2. All metadata are the same between old and new session.
//    For ANDROID_CONTROL_AE_TARGET_FPS_RANGE, only compare max fps.
bool IsSessionParameterCompatible(const HalCameraMetadata* old_session,
                                  const HalCameraMetadata* new_session);

}  // namespace utils
}  // namespace google_camera_hal
}  // namespace android

#endif  // HARDWARE_GOOGLE_CAMERA_HAL_UTILS_UTILS_H_
