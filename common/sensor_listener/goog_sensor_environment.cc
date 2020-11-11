/*
 * Copyright (C) 2018 The Android Open Source Project
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

#define LOG_TAG "goog_sensor_environment"

#include "utils/Errors.h"
#include "utils/Log.h"
#include "utils/RefBase.h"

#include "goog_sensor_environment.h"

namespace android {
namespace camera_sensor_listener {

using ::android::frameworks::sensorservice::V1_0::ISensorManager;
using ::android::frameworks::sensorservice::V1_0::Result;
using ::android::hardware::sensors::V1_0::SensorInfo;
using ::android::hardware::sensors::V1_0::SensorType;

GoogSensorEnvironment::GoogSensorEnvironment(
    EnvironmentSensorType environment_sensor_type, size_t event_queue_size)
    : GoogSensorWrapper(event_queue_size) {
  environment_sensor_type_index_ = static_cast<int>(environment_sensor_type);
  switch (environment_sensor_type) {
    case EnvironmentSensorType::DEVICE_ORIENTATION: {
      sensor_type_ = SensorType::DEVICE_ORIENTATION;
      break;
    }
    case EnvironmentSensorType::LIGHT: {
      sensor_type_ = SensorType::LIGHT;
      break;
    }
    case EnvironmentSensorType::PROXIMITY: {
      sensor_type_ = SensorType::PROXIMITY;
      break;
    }
    default: {
      break;
    }
  }
  ALOGI("%s %d create sensor %s", __func__, __LINE__,
        kEnvironmentSensorName[environment_sensor_type_index_]);
}

GoogSensorEnvironment::~GoogSensorEnvironment() {
  Disable();
  ALOGI("%s %d destroy sensor %s", __func__, __LINE__, GetSensorName());
}

sp<GoogSensorEnvironment> GoogSensorEnvironment::Create(
    EnvironmentSensorType environment_sensor_type, size_t event_queue_size) {
  // Check sensor_type validity.
  int environment_sensor_type_index = static_cast<int>(environment_sensor_type);
  if (environment_sensor_type_index >=
      static_cast<int>(EnvironmentSensorType::TOTAL_NUM)) {
    ALOGE("%s %d unsupported environment sensor type %d", __func__, __LINE__,
          environment_sensor_type_index);
    return nullptr;
  }

  // Create sensor.
  sp<GoogSensorEnvironment> sensor_ptr =
      new GoogSensorEnvironment(environment_sensor_type, event_queue_size);
  if (sensor_ptr == nullptr) {
    ALOGE("%s %d failed to create GoogSensorEnvironment for %s", __func__,
          __LINE__, kEnvironmentSensorName[environment_sensor_type_index]);
  } else {
    // Enable sensor.
    status_t result = sensor_ptr->Enable();
    if (result != 0) {
      ALOGE("%s %d failed to enable GoogSensorEnvironment for %s", __func__,
            __LINE__, sensor_ptr->GetSensorName());
    } else {
      ALOGI("%s %d successfully enabled GoogSensorEnvironment for %s", __func__,
            __LINE__, sensor_ptr->GetSensorName());
    }
  }
  return sensor_ptr;
}

void GoogSensorEnvironment::GetLatestNSensorEvents(
    int num_sample, std::vector<int64_t>* event_timestamps,
    std::vector<float>* event_data,
    std::vector<int64_t>* event_arrival_timestamps) const {
  event_timestamps->clear();
  event_data->clear();
  event_arrival_timestamps->clear();

  Mutex::Autolock l(event_deque_lock_);
  if (!event_deque_.empty()) {
    for (auto event = event_deque_.crbegin(); event != event_deque_.crend();
         ++event) {
      auto timestamp_iter = event_timestamps->begin();
      event_timestamps->insert(timestamp_iter, event->sensor_event.timestamp);
      auto data_iter = event_data->begin();
      event_data->insert(data_iter, event->sensor_event.u.scalar);
      auto arrival_iter = event_arrival_timestamps->begin();
      event_arrival_timestamps->insert(arrival_iter,
                                       event->event_arrival_time_ns);
      if (event_arrival_timestamps->size() >= num_sample) {
        break;
      }
    }
  }
}

int32_t GoogSensorEnvironment::GetSensorHandle() {
  sp<ISensorManager> manager = ISensorManager::getService();
  if (manager == nullptr) {
    ALOGE("%s %d Cannot get ISensorManager for sensor %s", __func__, __LINE__,
          GetSensorName());
    return -1;
  }
  bool found = false;
  SensorInfo sensor;
  manager->getDefaultSensor(
      sensor_type_, [&sensor, &found](const auto& info, auto result) {
        if (result != Result::OK) {
          ALOGE("%s %d Cannot find default sensor", __func__, __LINE__);
          return;
        }
        sensor = info;
        found = true;
      });

  if (found) {
    ALOGI("%s %d handle for %s is found.", __func__, __LINE__, GetSensorName());
  } else {
    ALOGE("%s %d handle for %s is not found!", __func__, __LINE__,
          GetSensorName());
  }
  return found ? sensor.sensorHandle : -1;
}

}  // namespace camera_sensor_listener
}  // namespace android