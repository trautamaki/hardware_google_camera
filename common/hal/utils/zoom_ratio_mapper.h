/*
 * Copyright (C) 2020 The Android Open Source Project
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

#ifndef HARDWARE_GOOGLE_CAMERA_HAL_UTILS_ZOOM_RATIO_MAPPER_H_
#define HARDWARE_GOOGLE_CAMERA_HAL_UTILS_ZOOM_RATIO_MAPPER_H_

#include "hal_types.h"

namespace android {
namespace google_camera_hal {

class ZoomRatioMapper {
 public:
  struct InitParams {
    Dimension active_array_dimension;
    ZoomRatioRange zoom_ratio_range;
  };

  void Initialize(InitParams params);

  // Apply zoom ratio to the capture request or result.
  void ApplyZoomRatio(HalCameraMetadata* metadata, const bool is_request);

 private:
  // Update crop region.
  void UpdateCropRegion(HalCameraMetadata* metadata, const float zoom_ratio,
                        const bool is_request);

  // Update AE/AF/AWB regions.
  void Update3ARegion(HalCameraMetadata* metadata, const float zoom_ratio,
                      const uint32_t tag_id, const bool is_request);

  // Update face retangles.
  void UpdateFaceRectangles(HalCameraMetadata* metadata, const float zoom_ratio);

  // Update face landmarks.
  void UpdateFaceLandmarks(HalCameraMetadata* metadata, const float zoom_ratio);

  // Map the rectangle to the coordination of HAL.
  void ConvertZoomRatio(const float zoom_ratio, int32_t* left, int32_t* top,
                        int32_t* width, int32_t* height);

  // Map the rectangle to the coordination of framework.
  void RevertZoomRatio(const float zoom_ratio, int32_t* left, int32_t* top,
                       int32_t* width, int32_t* height);

  // Map the position to the coordination of framework.
  void RevertZoomRatio(const float zoom_ratio, Point* new_point,
                       const Point* point);

  // Boundary check.
  void CorrectBoundary(int32_t* left, int32_t* top, int32_t* width,
                       int32_t* height, int32_t bound_w, int32_t bound_h);

  // Active array dimension.
  Dimension active_array_dimension_;

  // Zoom ratio range.
  ZoomRatioRange zoom_ratio_range_;

  // Indicate whether zoom ratio is supported.
  bool is_zoom_ratio_supported_ = false;
};

}  // namespace google_camera_hal
}  // namespace android

#endif  // HARDWARE_GOOGLE_CAMERA_HAL_UTILS_ZOOM_RATIO_MAPPER_H_
