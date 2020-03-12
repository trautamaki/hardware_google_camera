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

#define LOG_TAG "GCH_ZoomRatioMapper"

#include "zoom_ratio_mapper.h"
#include <log/log.h>

namespace android {
namespace google_camera_hal {

void ZoomRatioMapper::Initialize(InitParams params) {
  memcpy(&active_array_dimension_, &params.active_array_dimension,
         sizeof(active_array_dimension_));
  memcpy(&zoom_ratio_range_, &params.zoom_ratio_range,
         sizeof(zoom_ratio_range_));
  is_zoom_ratio_supported_ = true;
}

void ZoomRatioMapper::ApplyZoomRatio(HalCameraMetadata* metadata,
                                     const bool is_request) {
  if (metadata == nullptr) {
    ALOGE("%s: metadata is nullptr", __FUNCTION__);
    return;
  }

  if (!is_zoom_ratio_supported_) {
    ALOGV("%s: zoom ratio is not supported", __FUNCTION__);
    return;
  }

  camera_metadata_ro_entry entry = {};
  status_t res = metadata->Get(ANDROID_CONTROL_ZOOM_RATIO, &entry);
  if (res != OK) {
    ALOGE("%s: Failed to get the zoom ratio", __FUNCTION__);
    return;
  }
  float zoom_ratio = entry.data.f[0];

  if (zoom_ratio < zoom_ratio_range_.min) {
    ALOGE("%s, zoom_ratio(%f) is smaller than lower bound(%f)", __FUNCTION__,
          zoom_ratio, zoom_ratio_range_.min);
    zoom_ratio = zoom_ratio_range_.min;
  } else if (zoom_ratio > zoom_ratio_range_.max) {
    ALOGE("%s, zoom_ratio(%f) is larger than upper bound(%f)", __FUNCTION__,
          zoom_ratio, zoom_ratio_range_.max);
    zoom_ratio = zoom_ratio_range_.max;
  }

  UpdateCropRegion(metadata, zoom_ratio, is_request);
  Update3ARegion(metadata, zoom_ratio, ANDROID_CONTROL_AE_REGIONS, is_request);
  Update3ARegion(metadata, zoom_ratio, ANDROID_CONTROL_AF_REGIONS, is_request);
  Update3ARegion(metadata, zoom_ratio, ANDROID_CONTROL_AWB_REGIONS, is_request);

  if (!is_request) {
    uint8_t face_detection_mode = ANDROID_STATISTICS_FACE_DETECT_MODE_OFF;
    res = metadata->Get(ANDROID_STATISTICS_FACE_DETECT_MODE, &entry);
    if (res != OK) {
      ALOGE("%s: Failed to get face detection mode", __FUNCTION__);
      return;
    }
    face_detection_mode = *entry.data.u8;

    switch (face_detection_mode) {
      case ANDROID_STATISTICS_FACE_DETECT_MODE_FULL:
        UpdateFaceLandmarks(metadata, zoom_ratio);
        [[fallthrough]];
      case ANDROID_STATISTICS_FACE_DETECT_MODE_SIMPLE:
        UpdateFaceRectangles(metadata, zoom_ratio);
        break;
      default:
        break;
    }
  }
}

void ZoomRatioMapper::UpdateCropRegion(HalCameraMetadata* metadata,
                                       const float zoom_ratio,
                                       const bool is_request) {
  if (metadata == nullptr) {
    ALOGE("%s: metadata is nullptr", __FUNCTION__);
    return;
  }
  camera_metadata_ro_entry entry = {};
  status_t res = metadata->Get(ANDROID_SCALER_CROP_REGION, &entry);
  if (res != OK || entry.count == 0) {
    ALOGE("%s: Failed to get the region: %d, res: %d", __FUNCTION__,
          ANDROID_SCALER_CROP_REGION, res);
    return;
  }
  int32_t left = entry.data.i32[0];
  int32_t top = entry.data.i32[1];
  int32_t width = entry.data.i32[2];
  int32_t height = entry.data.i32[3];

  if (is_request) {
    ConvertZoomRatio(zoom_ratio, &left, &top, &width, &height);
  } else {
    RevertZoomRatio(zoom_ratio, &left, &top, &width, &height);
  }
  int32_t rect[4] = {left, top, width, height};

  ALOGV("%s: set ANDROID_SCALER_CROP_REGION: [%d, %d, %d, %d]", __FUNCTION__,
        rect[0], rect[1], rect[2], rect[3]);

  res = metadata->Set(ANDROID_SCALER_CROP_REGION, rect,
                      sizeof(rect) / sizeof(int32_t));
  if (res != OK) {
    ALOGE("%s: Updating crop region failed: %s (%d)", __FUNCTION__,
          strerror(-res), res);
  }
}

void ZoomRatioMapper::Update3ARegion(HalCameraMetadata* metadata,
                                     const float zoom_ratio,
                                     const uint32_t tag_id,
                                     const bool is_request) {
  if (metadata == nullptr) {
    ALOGE("%s: metadata is nullptr", __FUNCTION__);
    return;
  }
  camera_metadata_ro_entry entry = {};
  status_t res = metadata->Get(tag_id, &entry);
  if (res != OK || entry.count == 0) {
    ALOGV("%s: Failed to get the region: %d, res: %d", __FUNCTION__, tag_id,
          res);
    return;
  }
  const WeightedRect* regions =
      reinterpret_cast<const WeightedRect*>(entry.data.i32);
  const size_t kNumElementsInTuple = sizeof(WeightedRect) / sizeof(int32_t);
  std::vector<WeightedRect> updated_regions(entry.count / kNumElementsInTuple);

  for (size_t i = 0; i < updated_regions.size(); i++) {
    int32_t left = regions[i].left;
    int32_t top = regions[i].top;
    int32_t width = regions[i].right - regions[i].left + 1;
    int32_t height = regions[i].bottom - regions[i].top + 1;

    if (is_request) {
      ConvertZoomRatio(zoom_ratio, &left, &top, &width, &height);
    } else {
      RevertZoomRatio(zoom_ratio, &left, &top, &width, &height);
    }

    updated_regions[i].left = left;
    updated_regions[i].top = top;
    updated_regions[i].right = left + width - 1;
    updated_regions[i].bottom = top + height - 1;
    updated_regions[i].weight = regions[i].weight;

    ALOGV("%s: set 3A(%d) region: [%d, %d, %d, %d, %d]", __FUNCTION__, tag_id,
          updated_regions[i].left, updated_regions[i].top,
          updated_regions[i].right, updated_regions[i].bottom,
          updated_regions[i].weight);
  }
  res = metadata->Set(tag_id, reinterpret_cast<int32_t*>(updated_regions.data()),
                      updated_regions.size() * kNumElementsInTuple);
  if (res != OK) {
    ALOGE("%s: Updating region(%d) failed: %s (%d)", __FUNCTION__, tag_id,
          strerror(-res), res);
  }
}

void ZoomRatioMapper::ConvertZoomRatio(const float zoom_ratio, int32_t* left,
                                       int32_t* top, int32_t* width,
                                       int32_t* height) {
  if (left == nullptr || top == nullptr || width == nullptr ||
      height == nullptr) {
    ALOGE("%s, invalid params", __FUNCTION__);
    return;
  }

  assert(zoom_ratio != 0);
  *left = std::round(*left / zoom_ratio + 0.5f * active_array_dimension_.width *
                                              (1.0f - 1.0f / zoom_ratio));
  *top = std::round(*top / zoom_ratio + 0.5f * active_array_dimension_.height *
                                            (1.0f - 1.0f / zoom_ratio));
  *width = std::round(*width / zoom_ratio);
  *height = std::round(*height / zoom_ratio);

  if (zoom_ratio >= 1.0f) {
    CorrectBoundary(left, top, width, height, active_array_dimension_.width,
                    active_array_dimension_.height);
  }

  ALOGV("%s: zoom: %f, active array: [%d x %d], rect: [%d, %d, %d, %d]",
        __FUNCTION__, zoom_ratio, active_array_dimension_.width,
        active_array_dimension_.height, *left, *top, *width, *height);
}

void ZoomRatioMapper::RevertZoomRatio(const float zoom_ratio, int32_t* left,
                                      int32_t* top, int32_t* width,
                                      int32_t* height) {
  if (left == nullptr || top == nullptr || width == nullptr ||
      height == nullptr) {
    ALOGE("%s, invalid params", __FUNCTION__);
    return;
  }

  *left = std::round(*left * zoom_ratio - 0.5f * active_array_dimension_.width *
                                              (zoom_ratio - 1.0f));
  *top = std::round(*top * zoom_ratio - 0.5f * active_array_dimension_.height *
                                            (zoom_ratio - 1.0f));
  *width = std::round(*width * zoom_ratio);
  *height = std::round(*height * zoom_ratio);

  CorrectBoundary(left, top, width, height, active_array_dimension_.width,
                  active_array_dimension_.height);

  ALOGV("%s: zoom: %f, active array: [%d x %d], rect: [%d, %d, %d, %d]",
        __FUNCTION__, zoom_ratio, active_array_dimension_.width,
        active_array_dimension_.height, *left, *top, *width, *height);
}

void ZoomRatioMapper::CorrectBoundary(int32_t* left, int32_t* top,
                                      int32_t* width, int32_t* height,
                                      int32_t bound_w, int32_t bound_h) {
  if (left == nullptr || top == nullptr || width == nullptr ||
      height == nullptr) {
    ALOGE("%s, invalid params", __FUNCTION__);
    return;
  }
  *left = std::max(*left, 0);
  *left = std::min(*left, bound_w - 1);

  *top = std::max(*top, 0);
  *top = std::min(*top, bound_h - 1);

  *width = std::min(*width, bound_w - *left);

  *height = std::min(*height, bound_h - *top);
}

void ZoomRatioMapper::UpdateFaceRectangles(HalCameraMetadata* metadata,
                                           const float zoom_ratio) {
  if (metadata == nullptr) {
    ALOGE("%s: metadata is nullptr", __FUNCTION__);
    return;
  }
  camera_metadata_ro_entry entry = {};
  if (metadata->Get(ANDROID_STATISTICS_FACE_RECTANGLES, &entry) != OK) {
    ALOGV("%s: ANDROID_STATISTICS_FACE_RECTANGLES not published.", __FUNCTION__);
    return;
  }
  if (entry.count <= 0) {
    ALOGV("No face found.");
    return;
  }

  const Rect* face_rect = reinterpret_cast<const Rect*>(entry.data.i32);
  const size_t kNumElementsInTuple = sizeof(Rect) / sizeof(int32_t);
  std::vector<Rect> updated_face_rect(entry.count / kNumElementsInTuple);

  for (size_t i = 0; i < updated_face_rect.size(); i++) {
    int32_t left = face_rect[i].left;
    int32_t top = face_rect[i].top;
    int32_t width = face_rect[i].right - face_rect[i].left + 1;
    int32_t height = face_rect[i].bottom - face_rect[i].top + 1;

    RevertZoomRatio(zoom_ratio, &left, &top, &width, &height);

    updated_face_rect[i].left = left;
    updated_face_rect[i].top = top;
    updated_face_rect[i].right = left + width - 1;
    updated_face_rect[i].bottom = top + height - 1;

    ALOGV("%s: update face rectangle [%d, %d, %d, %d] -> [%d, %d, %d, %d]",
          __FUNCTION__, face_rect[i].left, face_rect[i].top, face_rect[i].right,
          face_rect[i].bottom, updated_face_rect[i].left,
          updated_face_rect[i].top, updated_face_rect[i].right,
          updated_face_rect[i].bottom);
  }
  status_t res =
      metadata->Set(ANDROID_STATISTICS_FACE_RECTANGLES,
                    reinterpret_cast<int32_t*>(updated_face_rect.data()),
                    updated_face_rect.size() * kNumElementsInTuple);
  if (res != OK) {
    ALOGE("%s: Updating face rectangle failed: %s (%d)", __FUNCTION__,
          strerror(-res), res);
  }
}

void ZoomRatioMapper::UpdateFaceLandmarks(HalCameraMetadata* metadata,
                                          const float zoom_ratio) {
  if (metadata == nullptr) {
    ALOGE("%s: metadata is nullptr", __FUNCTION__);
    return;
  }
  camera_metadata_ro_entry entry = {};
  if (metadata->Get(ANDROID_STATISTICS_FACE_LANDMARKS, &entry) != OK) {
    ALOGV("%s: ANDROID_STATISTICS_FACE_LANDMARKS not published.", __FUNCTION__);
    return;
  }

  if (entry.count <= 0) {
    ALOGV("No face landmarks found.");
    return;
  }

  // Each landmark is composed of 6 integers
  int32_t num_landmarks = entry.count / 6;
  uint32_t data_index = 0;
  uint32_t num_visible_landmarks = 0;
  std::vector<Point> landmarks(num_landmarks * 3);

  for (int32_t i = 0; i < num_landmarks; i++) {
    Point* transformed = &landmarks[num_visible_landmarks * 3];
    const Point points[3] = {
        {.x = static_cast<uint32_t>(entry.data.i32[data_index++]),
         .y = static_cast<uint32_t>(entry.data.i32[data_index++])},
        {.x = static_cast<uint32_t>(entry.data.i32[data_index++]),
         .y = static_cast<uint32_t>(entry.data.i32[data_index++])},
        {.x = static_cast<uint32_t>(entry.data.i32[data_index++]),
         .y = static_cast<uint32_t>(entry.data.i32[data_index++])}};

    RevertZoomRatio(zoom_ratio, &transformed[0], &points[0]);
    RevertZoomRatio(zoom_ratio, &transformed[1], &points[1]);
    RevertZoomRatio(zoom_ratio, &transformed[2], &points[2]);
    ALOGV(
        "%s: update face landmark: "
        "x_y(%d, %d), x_y(%d, %d), x_y(%d, %d) --> "
        "x_y(%d, %d), x_y(%d, %d), x_y(%d, %d).",
        __FUNCTION__, points[0].x, points[0].y, points[1].x, points[1].y,
        points[2].x, points[2].y, transformed[0].x, transformed[0].y,
        transformed[1].x, transformed[1].y, transformed[2].x, transformed[2].y);

    num_visible_landmarks++;
  }

  status_t res = metadata->Set(
      ANDROID_STATISTICS_FACE_LANDMARKS,
      reinterpret_cast<int32_t*>(landmarks.data()),
      num_visible_landmarks * 3 * (sizeof(Point) / sizeof(int32_t)));

  if (res != OK) {
    ALOGE("%s: Updating face landmarks failed: %s (%d)", __FUNCTION__,
          strerror(-res), res);
  }
}

void ZoomRatioMapper::RevertZoomRatio(const float zoom_ratio, Point* new_point,
                                      const Point* point) {
  if (new_point == nullptr || point == nullptr) {
    ALOGE("%s, invalid params", __FUNCTION__);
    return;
  }
  new_point->x =
      std::round(point->x * zoom_ratio -
                 0.5f * active_array_dimension_.width * (zoom_ratio - 1.0f));
  new_point->y =
      std::round(point->y * zoom_ratio -
                 0.5f * active_array_dimension_.height * (zoom_ratio - 1.0f));

  ALOGV("%s: zoom: %f, active array: [%d x %d], point: [%d, %d]", __FUNCTION__,
        zoom_ratio, active_array_dimension_.width,
        active_array_dimension_.height, new_point->x, new_point->y);
}

}  // namespace google_camera_hal
}  // namespace android
