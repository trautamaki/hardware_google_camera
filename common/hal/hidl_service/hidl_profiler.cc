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
#define LOG_TAG "GCH_HidlProfiler"

#include "hidl_profiler.h"

#include <log/log.h>

#include <memory>
#include <mutex>
#include <utility>

#include "profiler.h"

namespace android {
namespace hardware {
namespace camera {
namespace implementation {
namespace {

using ::google::camera_common::Profiler;

// setprop key for profiling open/close camera
constexpr char kPropKeyProfileOpenClose[] =
    "persist.vendor.camera.profiler.open_close";

constexpr char kFirstFrame[] = "First frame";
constexpr char kHalTotal[] = "HAL Total";
constexpr char kIdleString[] = "<-- IDLE -->";
constexpr char kOverall[] = "Overall";

class HidlProfilerImpl : public HidlProfiler {
 public:
  HidlProfilerImpl(uint32_t camera_id, int32_t flag)
      : kCameraIdString("Cam" + std::to_string(camera_id)),
        kProfilerFlag(flag) {
  }

  std::unique_ptr<HidlScopedProfiler> MakeScopedProfiler(
      ScopedType type) override {
    std::lock_guard lock(api_mutex_);
    if (profiler_ == nullptr) {
      CreateProfilerLocked();
      if (profiler_ == nullptr) {
        return nullptr;
      }
    }

    IdleEndLocked();

    const char* name = nullptr;
    int32_t id = 0;
    switch (type) {
      case ScopedType::kOpen:
        name = "Open";
        has_camera_open_ = true;
        profiler_->SetUseCase(kCameraIdString + "-Open");
        break;
      case ScopedType::kConfigureStream:
        name = "ConfigureStream";
        if (!has_camera_open_) {
          profiler_->SetUseCase(kCameraIdString + "-Reconfiguration");
        }
        id = config_count_++;
        break;
      case ScopedType::kFlush:
        name = "Flush";
        profiler_->SetUseCase(kCameraIdString + "-Flush");
        id = flush_count_++;
        break;
      case ScopedType::kClose:
        name = "Close";
        profiler_->SetUseCase(kCameraIdString + "-Close");
        break;
      default:
        ALOGE("%s: Unknown type %d", __FUNCTION__, type);
        return nullptr;
    }
    return std::make_unique<HidlScopedProfiler>(
        profiler_, name, id, [this, type]() {
          std::lock_guard lock(api_mutex_);
          if (type == ScopedType::kClose) {
            DeleteProfilerLocked();
          } else {
            IdleStartLocked();
          }
        });
  }

  void FirstFrameStart() override {
    std::lock_guard lock(api_mutex_);
    IdleEndLocked();
    if (profiler_) {
      profiler_->Start(kFirstFrame, Profiler::kInvalidRequestId);
      profiler_->Start(kHalTotal, Profiler::kInvalidRequestId);
    }
  }

  void FirstFrameEnd() override {
    std::lock_guard lock(api_mutex_);
    if (profiler_) {
      profiler_->End(kFirstFrame, Profiler::kInvalidRequestId);
      profiler_->End(kHalTotal, Profiler::kInvalidRequestId);
      DeleteProfilerLocked();
    }
  }

 private:
  void CreateProfilerLocked() {
    profiler_ = Profiler::Create(kProfilerFlag);
    if (profiler_ == nullptr) {
      ALOGE("%s: Failed to create profiler", __FUNCTION__);
      return;
    }
    has_camera_open_ = false;
    config_count_ = 0;
    flush_count_ = 0;
    idle_count_ = 0;
    profiler_->SetDumpFilePrefix(
        "/data/vendor/camera/profiler/hidl_open_close_");
    profiler_->Start(kOverall, Profiler::kInvalidRequestId);
  }

  void DeleteProfilerLocked() {
    if (profiler_) {
      profiler_->End(kOverall, Profiler::kInvalidRequestId);
      profiler_ = nullptr;
    }
  }

  void IdleStartLocked() {
    if (profiler_) {
      profiler_->Start(kIdleString, idle_count_++);
    }
  }

  void IdleEndLocked() {
    if (profiler_ && idle_count_) {
      profiler_->End(kIdleString, idle_count_ - 1);
    }
  }

  const std::string kCameraIdString;
  const int32_t kProfilerFlag = Profiler::SetPropFlag::kDisable;

  std::mutex api_mutex_;
  std::shared_ptr<Profiler> profiler_;
  bool has_camera_open_;
  uint8_t config_count_;
  uint8_t flush_count_;
  uint8_t idle_count_;
};

class HidlProfilerMock : public HidlProfiler {
  std::unique_ptr<HidlScopedProfiler> MakeScopedProfiler(ScopedType) override {
    return nullptr;
  }

  void FirstFrameStart() override {
  }

  void FirstFrameEnd() override {
  }
};

}  // anonymous namespace

std::shared_ptr<HidlProfiler> HidlProfiler::Create(uint32_t camera_id) {
  int32_t flag = property_get_int32(kPropKeyProfileOpenClose, 0);
  if (flag == Profiler::SetPropFlag::kDisable) {
    return std::make_shared<HidlProfilerMock>();
  }
  // Use stopwatch flag to print result.
  if (flag & Profiler::SetPropFlag::kPrintBit) {
    flag |= Profiler::SetPropFlag::kStopWatch;
  }
  return std::make_shared<HidlProfilerImpl>(camera_id, flag);
}

HidlScopedProfiler::HidlScopedProfiler(std::shared_ptr<Profiler> profiler,
                                       const std::string name, int id,
                                       std::function<void()> end_callback)
    : profiler_(profiler),
      name_(std::move(name)),
      id_(id),
      end_callback_(end_callback) {
  profiler_->Start(name_, id_);
  profiler_->Start(kHalTotal, Profiler::kInvalidRequestId);
}

HidlScopedProfiler::~HidlScopedProfiler() {
  profiler_->End(kHalTotal, Profiler::kInvalidRequestId);
  profiler_->End(name_, id_);
  if (end_callback_) {
    end_callback_();
  }
}

}  // namespace implementation
}  // namespace camera
}  // namespace hardware
}  // namespace android
