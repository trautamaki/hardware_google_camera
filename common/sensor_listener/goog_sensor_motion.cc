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

#define LOG_TAG "goog_sensor_motion"

#include <inttypes.h>

#include "utils/Errors.h"
#include "utils/Log.h"
#include "utils/Mutex.h"
#include "utils/RefBase.h"
#include "utils/String16.h"

#include "goog_sensor_motion.h"

namespace android {
namespace camera_sensor_listener {

using ::android::frameworks::sensorservice::V1_0::ISensorManager;
using ::android::frameworks::sensorservice::V1_0::Result;
using ::android::hardware::sensors::V1_0::SensorInfo;
using ::android::hardware::sensors::V1_0::SensorType;

GoogSensorMotion::GoogSensorMotion(MotionSensorType motion_sensor_type,
                                   int64_t sampling_period_us,
                                   size_t event_queue_size)
    : GoogSensorWrapper(event_queue_size, sampling_period_us) {
  motion_sensor_type_index_ = static_cast<int>(motion_sensor_type);
  switch (motion_sensor_type) {
    case MotionSensorType::ACCELEROMETER: {
      sensor_type_ = SensorType::ACCELEROMETER;
      break;
    }
    case MotionSensorType::GRAVITY: {
      sensor_type_ = SensorType::GRAVITY;
      break;
    }
    case MotionSensorType::GYROSCOPE: {
      sensor_type_ = SensorType::GYROSCOPE;
      break;
    }
    case MotionSensorType::LINEAR_ACCELERATION: {
      sensor_type_ = SensorType::LINEAR_ACCELERATION;
      break;
    }
    case MotionSensorType::MAGNETIC_FIELD: {
      sensor_type_ = SensorType::MAGNETIC_FIELD;
      break;
    }
    default: {
      break;
    }
  }
  ALOGI("%s %d create sensor %s", __func__, __LINE__,
        kMotionSensorName[motion_sensor_type_index_]);
}

GoogSensorMotion::~GoogSensorMotion() {
  Disable();
  ALOGI("%s %d destroy sensor %s", __func__, __LINE__, GetSensorName());
}

sp<GoogSensorMotion> GoogSensorMotion::Create(MotionSensorType motion_sensor_type,
                                              int64_t sampling_period_us,
                                              size_t event_queue_size) {
  // Check sensor_type validity.
  int motion_sensor_type_index = static_cast<int>(motion_sensor_type);
  if (motion_sensor_type_index >= static_cast<int>(MotionSensorType::TOTAL_NUM)) {
    ALOGE("%s %d unsupported motion sensor type %d", __func__, __LINE__,
          motion_sensor_type_index);
    return nullptr;
  }
  // Check sampling period validity.
  if (sampling_period_us < kMinSamplingPeriodUs) {
    ALOGE("%s %d sampling period %" PRId64 "us is too small %s", __func__,
          __LINE__, sampling_period_us,
          "supported smallest sampling period is 2500us");
    return nullptr;
  }

  // Create sensor.
  sp<GoogSensorMotion> sensor_ptr = new GoogSensorMotion(
      motion_sensor_type, sampling_period_us, event_queue_size);
  if (sensor_ptr == nullptr) {
    ALOGE("%s %d failed to create GoogSensorMotion for %s", __func__, __LINE__,
          kMotionSensorName[motion_sensor_type_index]);
  } else {
    // Enable sensor.
    status_t result = sensor_ptr->Enable();
    if (result != 0) {
      ALOGE("%s %d failed to enable GoogSensorMotion for %s", __func__,
            __LINE__, sensor_ptr->GetSensorName());
    } else {
      ALOGI("%s %d successfully enabled GoogSensorMotion for %s", __func__,
            __LINE__, sensor_ptr->GetSensorName());
    }
  }
  return sensor_ptr;
}

void GoogSensorMotion::GetLatestNSensorEvents(
    int num_sample, std::vector<int64_t>* event_timestamps,
    std::vector<float>* motion_vector_x, std::vector<float>* motion_vector_y,
    std::vector<float>* motion_vector_z,
    std::vector<int64_t>* event_arrival_timestamps) const {
  event_timestamps->clear();
  motion_vector_x->clear();
  motion_vector_y->clear();
  motion_vector_z->clear();
  event_arrival_timestamps->clear();

  Mutex::Autolock l(event_deque_lock_);
  if (!event_deque_.empty()) {
    for (auto event = event_deque_.crbegin(); event != event_deque_.crend();
         ++event) {
      auto timestamp_iter = event_timestamps->begin();
      event_timestamps->insert(timestamp_iter, event->sensor_event.timestamp);
      auto vec_x_iter = motion_vector_x->begin();
      motion_vector_x->insert(vec_x_iter, event->sensor_event.u.vec3.x);
      auto vec_y_iter = motion_vector_y->begin();
      motion_vector_y->insert(vec_y_iter, event->sensor_event.u.vec3.y);
      auto vec_z_iter = motion_vector_z->begin();
      motion_vector_z->insert(vec_z_iter, event->sensor_event.u.vec3.z);
      auto arrival_iter = event_arrival_timestamps->begin();
      event_arrival_timestamps->insert(arrival_iter,
                                       event->event_arrival_time_ns);
      if (event_arrival_timestamps->size() >= num_sample) {
        break;
      }
    }
  }
}

void GoogSensorMotion::QuerySensorEventsBetweenTimestamps(
    int64_t start_time, int64_t end_time,
    std::vector<int64_t>* event_timestamps, std::vector<float>* motion_vector_x,
    std::vector<float>* motion_vector_y, std::vector<float>* motion_vector_z,
    std::vector<int64_t>* event_arrival_timestamps) const {
  event_timestamps->clear();
  motion_vector_x->clear();
  motion_vector_y->clear();
  motion_vector_z->clear();
  event_arrival_timestamps->clear();

  Mutex::Autolock l(event_deque_lock_);
  for (const auto& event : event_deque_) {
    int64_t event_time = event.sensor_event.timestamp;
    if (event_time <= start_time || event_time > end_time) {
      continue;
    }
    event_timestamps->push_back(event_time);
    motion_vector_x->push_back(event.sensor_event.u.vec3.x);
    motion_vector_y->push_back(event.sensor_event.u.vec3.y);
    motion_vector_z->push_back(event.sensor_event.u.vec3.z);
    event_arrival_timestamps->push_back(event.event_arrival_time_ns);
  }
}

int32_t GoogSensorMotion::GetSensorHandle() {
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