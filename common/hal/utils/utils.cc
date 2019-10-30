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
#define LOG_TAG "GCH_Utils"

#include "utils.h"

#include <hardware/gralloc.h>
#include <log/log.h>

#include "vendor_tag_defs.h"

namespace android {
namespace google_camera_hal {
namespace utils {

bool IsDepthStream(const Stream& stream) {
  if (stream.stream_type == StreamType::kOutput &&
      stream.data_space == HAL_DATASPACE_DEPTH &&
      stream.format == HAL_PIXEL_FORMAT_Y16) {
    return true;
  }

  return false;
}

bool IsPreviewStream(const Stream& stream) {
  if (stream.stream_type == StreamType::kOutput &&
      stream.format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED &&
      ((stream.usage & GRALLOC_USAGE_HW_COMPOSER) == GRALLOC_USAGE_HW_COMPOSER ||
       (stream.usage & GRALLOC_USAGE_HW_TEXTURE) == GRALLOC_USAGE_HW_TEXTURE)) {
    return true;
  }

  return false;
}

bool IsJPEGSnapshotStream(const Stream& stream) {
  if (stream.stream_type == StreamType::kOutput &&
      stream.format == HAL_PIXEL_FORMAT_BLOB &&
      (stream.data_space == HAL_DATASPACE_JFIF ||
       stream.data_space == HAL_DATASPACE_V0_JFIF)) {
    return true;
  }

  return false;
}

bool IsVideoStream(const Stream& stream) {
  if (stream.stream_type == StreamType::kOutput &&
      (stream.usage & GRALLOC_USAGE_HW_VIDEO_ENCODER) != 0) {
    return true;
  }

  return false;
}

bool IsRawStream(const Stream& stream) {
  if (stream.stream_type == StreamType::kOutput &&
      (stream.format == HAL_PIXEL_FORMAT_RAW10 ||
       stream.format == HAL_PIXEL_FORMAT_RAW16 ||
       stream.format == HAL_PIXEL_FORMAT_RAW_OPAQUE)) {
    return true;
  }

  return false;
}

bool IsInputRawStream(const Stream& stream) {
  if (stream.stream_type == StreamType::kInput &&
      (stream.format == HAL_PIXEL_FORMAT_RAW10 ||
       stream.format == HAL_PIXEL_FORMAT_RAW16 ||
       stream.format == HAL_PIXEL_FORMAT_RAW_OPAQUE)) {
    return true;
  }

  return false;
}

bool IsArbitraryDataSpaceRawStream(const Stream& stream) {
  return IsRawStream(stream) && (stream.data_space == HAL_DATASPACE_ARBITRARY);
}

bool IsYUVSnapshotStream(const Stream& stream) {
  if (stream.stream_type == StreamType::kOutput &&
      stream.format == HAL_PIXEL_FORMAT_YCbCr_420_888 &&
      !IsVideoStream(stream) && !IsPreviewStream(stream)) {
    return true;
  }

  return false;
}

status_t GetSensorPhysicalSize(const HalCameraMetadata* characteristics,
                               float* width, float* height) {
  if (characteristics == nullptr || width == nullptr || height == nullptr) {
    ALOGE("%s: characteristics or width/height is nullptr", __FUNCTION__);
    return BAD_VALUE;
  }

  camera_metadata_ro_entry entry;
  status_t res = characteristics->Get(ANDROID_SENSOR_INFO_PHYSICAL_SIZE, &entry);
  if (res != OK || entry.count != 2) {
    ALOGE(
        "%s: Getting ANDROID_SENSOR_INFO_PHYSICAL_SIZE failed: %s(%d) count: "
        "%zu",
        __FUNCTION__, strerror(-res), res, entry.count);
    return res;
  }

  *width = entry.data.f[0];
  *height = entry.data.f[1];
  return OK;
}

status_t GetSensorActiveArraySize(const HalCameraMetadata* characteristics,
                                  Rect* active_array) {
  if (characteristics == nullptr || active_array == nullptr) {
    ALOGE("%s: characteristics or active_array is nullptr", __FUNCTION__);
    return BAD_VALUE;
  }

  camera_metadata_ro_entry entry;
  status_t res =
      characteristics->Get(ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE, &entry);
  if (res != OK || entry.count != 4) {
    ALOGE(
        "%s: Getting ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE failed: %s(%d) "
        "count: %zu",
        __FUNCTION__, strerror(-res), res, entry.count);
    return res;
  }

  active_array->left = entry.data.i32[0];
  active_array->top = entry.data.i32[1];
  active_array->right = entry.data.i32[0] + entry.data.i32[2] - 1;
  active_array->bottom = entry.data.i32[1] + entry.data.i32[3] - 1;

  return OK;
}

status_t GetSensorPixelArraySize(const HalCameraMetadata* characteristics,
                                 Dimension* pixel_array) {
  if (characteristics == nullptr || pixel_array == nullptr) {
    ALOGE("%s: characteristics or pixel_array is nullptr", __FUNCTION__);
    return BAD_VALUE;
  }

  camera_metadata_ro_entry entry;
  status_t res =
      characteristics->Get(ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE, &entry);
  if (res != OK || entry.count != 2) {
    ALOGE(
        "%s: Getting ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE failed: %s(%d) "
        "count: %zu",
        __FUNCTION__, strerror(-res), res, entry.count);
    return res;
  }

  pixel_array->width = entry.data.i32[0];
  pixel_array->height = entry.data.i32[1];

  return OK;
}

status_t GetFocalLength(const HalCameraMetadata* characteristics,
                        float* focal_length) {
  if (characteristics == nullptr || focal_length == nullptr) {
    ALOGE("%s: characteristics or focal_length is nullptr", __FUNCTION__);
    return BAD_VALUE;
  }

  camera_metadata_ro_entry entry;
  status_t res =
      characteristics->Get(ANDROID_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, &entry);
  if (res != OK || entry.count != 1) {
    ALOGE(
        "%s: Getting ANDROID_LENS_INFO_AVAILABLE_FOCAL_LENGTHS failed: %s(%d) "
        "count: %zu",
        __FUNCTION__, strerror(-res), res, entry.count);
    return res;
  }

  *focal_length = entry.data.f[0];

  return OK;
}

bool IsLiveSnapshotConfigured(const StreamConfiguration& stream_config) {
  bool has_video_stream = false;
  bool has_jpeg_stream = false;
  for (auto stream : stream_config.streams) {
    if (utils::IsVideoStream(stream)) {
      has_video_stream = true;
    } else if (utils::IsJPEGSnapshotStream(stream)) {
      has_jpeg_stream = true;
    }
  }

  return (has_video_stream & has_jpeg_stream);
}

}  // namespace utils
}  // namespace google_camera_hal
}  // namespace android
