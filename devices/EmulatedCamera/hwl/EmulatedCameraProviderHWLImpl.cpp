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
#define LOG_TAG "EmulatedCameraProviderHwlImpl"
#include "EmulatedCameraProviderHWLImpl.h"

#include <android-base/file.h>
#include <android-base/strings.h>
#include <cutils/properties.h>
#include <hardware/camera_common.h>
#include <log/log.h>

#include "EmulatedCameraDeviceHWLImpl.h"
#include "EmulatedCameraDeviceSessionHWLImpl.h"
#include "EmulatedLogicalRequestState.h"
#include "EmulatedSensor.h"
#include "EmulatedTorchState.h"
#include "utils/HWLUtils.h"
#include "vendor_tag_defs.h"

namespace android {

// Location of the camera configuration files.
const char* EmulatedCameraProviderHwlImpl::kConfigurationFileLocation[] = {
    "/vendor/etc/config/emu_camera_back.json",
    "/vendor/etc/config/emu_camera_front.json",
    "/vendor/etc/config/emu_camera_depth.json",
};

std::unique_ptr<EmulatedCameraProviderHwlImpl>
EmulatedCameraProviderHwlImpl::Create() {
  auto provider = std::unique_ptr<EmulatedCameraProviderHwlImpl>(
      new EmulatedCameraProviderHwlImpl());

  if (provider == nullptr) {
    ALOGE("%s: Creating EmulatedCameraProviderHwlImpl failed.", __FUNCTION__);
    return nullptr;
  }

  status_t res = provider->Initialize();
  if (res != OK) {
    ALOGE("%s: Initializing EmulatedCameraProviderHwlImpl failed: %s (%d).",
          __FUNCTION__, strerror(-res), res);
    return nullptr;
  }

  ALOGI("%s: Created EmulatedCameraProviderHwlImpl", __FUNCTION__);

  return provider;
}

status_t EmulatedCameraProviderHwlImpl::GetTagFromName(const char* name,
                                                       uint32_t* tag) {
  if (name == nullptr || tag == nullptr) {
    return BAD_VALUE;
  }

  size_t name_length = strlen(name);
  // First, find the section by the longest string match
  const char* section = NULL;
  size_t section_index = 0;
  size_t section_length = 0;
  for (size_t i = 0; i < ANDROID_SECTION_COUNT; ++i) {
    const char* str = camera_metadata_section_names[i];

    ALOGV("%s: Trying to match against section '%s'", __FUNCTION__, str);

    if (strstr(name, str) == name) {  // name begins with the section name
      size_t str_length = strlen(str);

      ALOGV("%s: Name begins with section name", __FUNCTION__);

      // section name is the longest we've found so far
      if (section == NULL || section_length < str_length) {
        section = str;
        section_index = i;
        section_length = str_length;

        ALOGV("%s: Found new best section (%s)", __FUNCTION__, section);
      }
    }
  }

  if (section == NULL) {
    return NAME_NOT_FOUND;
  } else {
    ALOGV("%s: Found matched section '%s' (%zu)", __FUNCTION__, section,
          section_index);
  }

  // Get the tag name component of the name
  const char* name_tag_name = name + section_length + 1;  // x.y.z -> z
  if (section_length + 1 >= name_length) {
    return BAD_VALUE;
  }

  // Match rest of name against the tag names in that section only
  uint32_t candidate_tag = 0;
  // Match built-in tags (typically android.*)
  uint32_t tag_begin, tag_end;  // [tag_begin, tag_end)
  tag_begin = camera_metadata_section_bounds[section_index][0];
  tag_end = camera_metadata_section_bounds[section_index][1];

  for (candidate_tag = tag_begin; candidate_tag < tag_end; ++candidate_tag) {
    const char* tag_name = get_camera_metadata_tag_name(candidate_tag);

    if (strcmp(name_tag_name, tag_name) == 0) {
      ALOGV("%s: Found matched tag '%s' (%d)", __FUNCTION__, tag_name,
            candidate_tag);
      break;
    }
  }

  if (candidate_tag == tag_end) {
    return NAME_NOT_FOUND;
  }

  *tag = candidate_tag;
  return OK;
}

bool IsDigit(const std::string& value) {
  if (value.empty()) {
    return false;
  }

  for (const auto& c : value) {
    if (!std::isdigit(c) && (!std::ispunct(c))) {
      return false;
    }
  }

  return true;
}

template <typename T>
status_t GetEnumValue(uint32_t tag_id, const char* cstring, T* result /*out*/) {
  if ((result == nullptr) || (cstring == nullptr)) {
    return BAD_VALUE;
  }

  uint32_t enum_value;
  auto ret =
      camera_metadata_enum_value(tag_id, cstring, strlen(cstring), &enum_value);
  if (ret != OK) {
    ALOGE("%s: Failed to match tag id: 0x%x value: %s", __FUNCTION__, tag_id,
          cstring);
    return ret;
  }
  *result = enum_value;

  return OK;
}

status_t GetUInt8Value(const Json::Value& value, uint32_t tag_id,
                       uint8_t* result /*out*/) {
  if (result == nullptr) {
    return BAD_VALUE;
  }

  if (value.isString()) {
    errno = 0;
    if (IsDigit(value.asString())) {
      auto int_value = strtol(value.asCString(), nullptr, 10);
      if ((int_value >= 0) && (int_value <= UINT8_MAX) && (errno == 0)) {
        *result = int_value;
      } else {
        ALOGE("%s: Failed parsing tag id 0x%x", __func__, tag_id);
        return BAD_VALUE;
      }
    } else {
      return GetEnumValue(tag_id, value.asCString(), result);
    }
  } else {
    ALOGE(
        "%s: Unexpected json type: %d! All value types are expected to be "
        "strings!",
        __FUNCTION__, value.type());
    return BAD_VALUE;
  }

  return OK;
}

status_t GetInt32Value(const Json::Value& value, uint32_t tag_id,
                       int32_t* result /*out*/) {
  if (result == nullptr) {
    return BAD_VALUE;
  }

  if (value.isString()) {
    errno = 0;
    if (IsDigit(value.asString())) {
      auto int_value = strtol(value.asCString(), nullptr, 10);
      if ((int_value >= INT32_MIN) && (int_value <= INT32_MAX) && (errno == 0)) {
        *result = int_value;
      } else {
        ALOGE("%s: Failed parsing tag id 0x%x", __func__, tag_id);
        return BAD_VALUE;
      }
    } else {
      return GetEnumValue(tag_id, value.asCString(), result);
    }
  } else {
    ALOGE(
        "%s: Unexpected json type: %d! All value types are expected to be "
        "strings!",
        __FUNCTION__, value.type());
    return BAD_VALUE;
  }

  return OK;
}

status_t GetInt64Value(const Json::Value& value, uint32_t tag_id,
                       int64_t* result /*out*/) {
  if (result == nullptr) {
    return BAD_VALUE;
  }

  if (value.isString()) {
    errno = 0;
    auto int_value = strtoll(value.asCString(), nullptr, 10);
    if ((int_value >= INT64_MIN) && (int_value <= INT64_MAX) && (errno == 0)) {
      *result = int_value;
    } else {
      ALOGE("%s: Failed parsing tag id 0x%x", __func__, tag_id);
      return BAD_VALUE;
    }
  } else {
    ALOGE(
        "%s: Unexpected json type: %d! All value types are expected to be "
        "strings!",
        __FUNCTION__, value.type());
    return BAD_VALUE;
  }

  return OK;
}

status_t GetFloatValue(const Json::Value& value, uint32_t tag_id,
                       float* result /*out*/) {
  if (result == nullptr) {
    return BAD_VALUE;
  }

  if (value.isString()) {
    errno = 0;
    auto float_value = strtof(value.asCString(), nullptr);
    if (errno == 0) {
      *result = float_value;
    } else {
      ALOGE("%s: Failed parsing tag id 0x%x", __func__, tag_id);
      return BAD_VALUE;
    }
  } else {
    ALOGE(
        "%s: Unexpected json type: %d! All value types are expected to be "
        "strings!",
        __FUNCTION__, value.type());
    return BAD_VALUE;
  }

  return OK;
}

status_t GetDoubleValue(const Json::Value& value, uint32_t tag_id,
                        double* result /*out*/) {
  if (result == nullptr) {
    return BAD_VALUE;
  }

  if (value.isString()) {
    errno = 0;
    auto double_value = strtod(value.asCString(), nullptr);
    if (errno == 0) {
      *result = double_value;
    } else {
      ALOGE("%s: Failed parsing tag id 0x%x", __func__, tag_id);
      return BAD_VALUE;
    }
  } else {
    ALOGE(
        "%s: Unexpected json type: %d! All value types are expected to be "
        "strings!",
        __FUNCTION__, value.type());
    return BAD_VALUE;
  }

  return OK;
}

template <typename T>
void FilterVendorKeys(uint32_t tag_id, std::vector<T>* values) {
  if ((values == nullptr) || (values->empty())) {
    return;
  }

  switch (tag_id) {
    case ANDROID_REQUEST_AVAILABLE_REQUEST_KEYS:
    case ANDROID_REQUEST_AVAILABLE_RESULT_KEYS:
    case ANDROID_REQUEST_AVAILABLE_SESSION_KEYS:
    case ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS: {
      auto it = values->begin();
      while (it != values->end()) {
        // Per spec. the tags we are handling here will be "int32_t".
        // In this case all vendor defined values will be negative.
        if (*it < 0) {
          it = values->erase(it);
        } else {
          it++;
        }
      }
    } break;
    default:
      // no-op
      break;
  }
}

template <typename T, typename func_type>
status_t InsertTag(const Json::Value& json_value, uint32_t tag_id,
                   func_type get_val_func, HalCameraMetadata* meta /*out*/) {
  if (meta == nullptr) {
    return BAD_VALUE;
  }

  std::vector<T> values;
  T result;
  status_t ret = -1;
  values.reserve(json_value.size());
  for (const auto& val : json_value) {
    ret = get_val_func(val, tag_id, &result);
    if (ret != OK) {
      break;
    }

    values.push_back(result);
  }

  if (ret == OK) {
    FilterVendorKeys(tag_id, &values);
    ret = meta->Set(tag_id, values.data(), values.size());
  }

  return ret;
}

status_t InsertRationalTag(const Json::Value& json_value, uint32_t tag_id,
                           HalCameraMetadata* meta /*out*/) {
  if (meta == nullptr) {
    return BAD_VALUE;
  }

  std::vector<camera_metadata_rational_t> values;
  status_t ret = OK;
  if (json_value.isArray() && ((json_value.size() % 2) == 0)) {
    values.reserve(json_value.size() / 2);
    auto it = json_value.begin();
    while (it != json_value.end()) {
      camera_metadata_rational_t result;
      ret = GetInt32Value((*it), tag_id, &result.numerator);
      it++;
      ret |= GetInt32Value((*it), tag_id, &result.denominator);
      it++;
      if (ret != OK) {
        break;
      }
      values.push_back(result);
    }
  } else {
    ALOGE("%s: json type: %d doesn't match with rational tag type",
          __FUNCTION__, json_value.type());
    return BAD_VALUE;
  }

  if (ret == OK) {
    ret = meta->Set(tag_id, values.data(), values.size());
  }

  return ret;
}

uint32_t EmulatedCameraProviderHwlImpl::ParseCharacteristics(
    const Json::Value& value, ssize_t id) {
  if (!value.isObject()) {
    ALOGE("%s: Configuration root is not an object", __FUNCTION__);
    return BAD_VALUE;
  }

  auto static_meta = HalCameraMetadata::Create(1, 10);
  auto members = value.getMemberNames();
  for (const auto& member : members) {
    uint32_t tag_id;
    auto stat = GetTagFromName(member.c_str(), &tag_id);
    if (stat != OK) {
      ALOGE("%s: tag %s not supported, skipping!", __func__, member.c_str());
      continue;
    }

    auto tag_type = get_camera_metadata_tag_type(tag_id);
    auto tag_value = value[member.c_str()];
    switch (tag_type) {
      case TYPE_BYTE:
        InsertTag<uint8_t>(tag_value, tag_id, GetUInt8Value, static_meta.get());
        break;
      case TYPE_INT32:
        InsertTag<int32_t>(tag_value, tag_id, GetInt32Value, static_meta.get());
        break;
      case TYPE_INT64:
        InsertTag<int64_t>(tag_value, tag_id, GetInt64Value, static_meta.get());
        break;
      case TYPE_FLOAT:
        InsertTag<float>(tag_value, tag_id, GetFloatValue, static_meta.get());
        break;
      case TYPE_DOUBLE:
        InsertTag<double>(tag_value, tag_id, GetDoubleValue, static_meta.get());
        break;
      case TYPE_RATIONAL:
        InsertRationalTag(tag_value, tag_id, static_meta.get());
        break;
      default:
        ALOGE("%s: Unsupported tag type: %d!", __FUNCTION__, tag_type);
    }
  }

  SensorCharacteristics sensor_characteristics;
  auto ret =
      GetSensorCharacteristics(static_meta.get(), &sensor_characteristics);
  if (ret != OK) {
    ALOGE("%s: Unable to extract sensor characteristics!", __FUNCTION__);
    return ret;
  }

  if (!EmulatedSensor::AreCharacteristicsSupported(sensor_characteristics)) {
    ALOGE("%s: Sensor characteristics not supported!", __FUNCTION__);
    return BAD_VALUE;
  }

  // Although we don't support HdrPlus, this data is still required by HWL
  int32_t payload_frames = 0;
  static_meta->Set(google_camera_hal::kHdrplusPayloadFrames, &payload_frames, 1);

  if (id < 0) {
    static_metadata_.push_back(std::move(static_meta));
    id = static_metadata_.size() - 1;
  } else {
    static_metadata_[id] = std::move(static_meta);
  }

  return id;
}

status_t EmulatedCameraProviderHwlImpl::WaitForQemuSfFakeCameraPropertyAvailable() {
  // Camera service may start running before qemu-props sets
  // qemu.sf.fake_camera to any of the follwing four values:
  // "none,front,back,both"; so we need to wait.
  int num_attempts = 100;
  char prop[PROPERTY_VALUE_MAX];
  bool timeout = true;
  for (int i = 0; i < num_attempts; ++i) {
    if (property_get("qemu.sf.fake_camera", prop, nullptr) != 0) {
      timeout = false;
      break;
    }
    usleep(5000);
  }
  if (timeout) {
    ALOGE("timeout (%dms) waiting for property qemu.sf.fake_camera to be set\n",
          5 * num_attempts);
    return BAD_VALUE;
  }
  return OK;
}

status_t EmulatedCameraProviderHwlImpl::Initialize() {
  // GCH expects all physical ids to be bigger than the logical ones.
  // Resize 'static_metadata_' to fit all logical devices and insert them
  // accordingly, push any remaining physical cameras in the back.
  std::string config;
  size_t logical_id = 0;
  std::vector<const char*> configurationFileLocation;
  char prop[PROPERTY_VALUE_MAX];
  if (!property_get_bool("ro.kernel.qemu", false)) {
    for (const auto& iter : kConfigurationFileLocation) {
      configurationFileLocation.emplace_back(iter);
    }
  } else {
    // Android Studio Emulator
    if (!property_get_bool("ro.kernel.qemu.legacy_fake_camera", false)) {
      if (WaitForQemuSfFakeCameraPropertyAvailable() == OK) {
        property_get("qemu.sf.fake_camera", prop, nullptr);
        if (strcmp(prop, "both") == 0) {
          configurationFileLocation.emplace_back(kConfigurationFileLocation[0]);
          configurationFileLocation.emplace_back(kConfigurationFileLocation[1]);
        } else if (strcmp(prop, "front") == 0) {
          configurationFileLocation.emplace_back(kConfigurationFileLocation[1]);
          logical_id = 1;
        } else if (strcmp(prop, "back") == 0) {
          configurationFileLocation.emplace_back(kConfigurationFileLocation[0]);
          logical_id = 1;
        }
      }
    }
  }
  static_metadata_.resize(sizeof(configurationFileLocation));

  for (const auto& config_path : configurationFileLocation) {
    if (!android::base::ReadFileToString(config_path, &config)) {
      ALOGW("%s: Could not open configuration file: %s", __FUNCTION__,
            config_path);
      continue;
    }

    Json::Reader config_reader;
    Json::Value root;
    if (!config_reader.parse(config, root)) {
      ALOGE("Could not parse configuration file: %s",
            config_reader.getFormattedErrorMessages().c_str());
      return BAD_VALUE;
    }

    if (root.isArray()) {
      auto device_iter = root.begin();
      auto result_id = ParseCharacteristics(*device_iter, logical_id);
      if (logical_id != result_id) {
        return result_id;
      }
      device_iter++;

      // The first device entry is always the logical camera followed by the
      // physical devices. They must be at least 2.
      camera_id_map_.emplace(logical_id, std::vector<uint32_t>());
      if (root.size() >= 3) {
        camera_id_map_[logical_id].reserve(root.size() - 1);
        while (device_iter != root.end()) {
          auto physical_id = ParseCharacteristics(*device_iter, /*id*/ -1);
          if (physical_id < 0) {
            return physical_id;
          }
          camera_id_map_[logical_id].push_back(physical_id);

          device_iter++;
        }

        auto physical_devices = std::make_unique<PhysicalDeviceMap>();
        for (const auto& physical_device_id : camera_id_map_[logical_id]) {
          physical_devices->emplace(
              physical_device_id,
              HalCameraMetadata::Clone(
                  static_metadata_[physical_device_id].get()));
        }
        auto updated_logical_chars =
            EmulatedLogicalRequestState::AdaptLogicalCharacteristics(
                HalCameraMetadata::Clone(static_metadata_[logical_id].get()),
                std::move(physical_devices));
        if (updated_logical_chars.get() != nullptr) {
          static_metadata_[logical_id].swap(updated_logical_chars);
        } else {
          ALOGE("%s: Failed to updating logical camera characteristics!",
                __FUNCTION__);
          return BAD_VALUE;
        }
      }
    } else {
      auto result_id = ParseCharacteristics(root, logical_id);
      if (result_id != logical_id) {
        return result_id;
      }
      camera_id_map_.emplace(logical_id, std::vector<uint32_t>());
    }

    logical_id++;
  }

  return OK;
}

status_t EmulatedCameraProviderHwlImpl::SetCallback(
    const HwlCameraProviderCallback& callback) {
  torch_cb_ = callback.torch_mode_status_change;

  return OK;
}

status_t EmulatedCameraProviderHwlImpl::GetVendorTags(
    std::vector<VendorTagSection>* vendor_tag_sections) {
  if (vendor_tag_sections == nullptr) {
    ALOGE("%s: vendor_tag_sections is nullptr.", __FUNCTION__);
    return BAD_VALUE;
  }

  // No vendor specific tags as of now
  return OK;
}

status_t EmulatedCameraProviderHwlImpl::GetVisibleCameraIds(
    std::vector<std::uint32_t>* camera_ids) {
  if (camera_ids == nullptr) {
    ALOGE("%s: camera_ids is nullptr.", __FUNCTION__);
    return BAD_VALUE;
  }

  for (const auto& device : camera_id_map_) {
    camera_ids->push_back(device.first);
  }

  return OK;
}

status_t EmulatedCameraProviderHwlImpl::CreateCameraDeviceHwl(
    uint32_t camera_id, std::unique_ptr<CameraDeviceHwl>* camera_device_hwl) {
  if (camera_device_hwl == nullptr) {
    ALOGE("%s: camera_device_hwl is nullptr.", __FUNCTION__);
    return BAD_VALUE;
  }

  if (camera_id_map_.find(camera_id) == camera_id_map_.end()) {
    ALOGE("%s: Invalid camera id: %u", __func__, camera_id);
    return BAD_VALUE;
  }

  std::unique_ptr<HalCameraMetadata> meta =
      HalCameraMetadata::Clone(static_metadata_[camera_id].get());

  std::shared_ptr<EmulatedTorchState> torch_state;
  camera_metadata_ro_entry entry;
  bool flash_supported = false;
  auto ret = meta->Get(ANDROID_FLASH_INFO_AVAILABLE, &entry);
  if ((ret == OK) && (entry.count == 1)) {
    if (entry.data.u8[0] == ANDROID_FLASH_INFO_AVAILABLE_TRUE) {
      flash_supported = true;
    }
  }

  if (flash_supported) {
    torch_state = std::make_shared<EmulatedTorchState>(camera_id, torch_cb_);
  }

  auto physical_devices = std::make_unique<PhysicalDeviceMap>();
  for (const auto& physical_device_id : camera_id_map_[camera_id]) {
    physical_devices->emplace(
        physical_device_id,
        HalCameraMetadata::Clone(static_metadata_[physical_device_id].get()));
  }
  *camera_device_hwl = EmulatedCameraDeviceHwlImpl::Create(
      camera_id, std::move(meta), std::move(physical_devices), torch_state);
  if (*camera_device_hwl == nullptr) {
    ALOGE("%s: Cannot create EmulatedCameraDeviceHWlImpl.", __FUNCTION__);
    return BAD_VALUE;
  }

  return OK;
}

status_t EmulatedCameraProviderHwlImpl::CreateBufferAllocatorHwl(
    std::unique_ptr<CameraBufferAllocatorHwl>* camera_buffer_allocator_hwl) {
  if (camera_buffer_allocator_hwl == nullptr) {
    ALOGE("%s: camera_buffer_allocator_hwl is nullptr.", __FUNCTION__);
    return BAD_VALUE;
  }

  // Currently not supported
  return INVALID_OPERATION;
}
}  // namespace android
