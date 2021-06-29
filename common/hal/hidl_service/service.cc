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

#ifdef LAZY_SERVICE
#define LOG_TAG "android.hardware.pixel.camera.provider@2.7-service-lazy"
#else
#define LOG_TAG "android.hardware.pixel.camera.provider@2.7-service"
#endif

#include <android/hardware/camera/provider/2.7/ICameraProvider.h>
#include <binder/ProcessState.h>
#include <hidl/HidlLazyUtils.h>
#include <hidl/HidlTransportSupport.h>
#include <malloc.h>
#include <utils/Errors.h>

#include "hidl_camera_provider.h"

using ::android::hardware::camera::provider::V2_7::ICameraProvider;
using ::android::hardware::camera::provider::V2_7::implementation::HidlCameraProvider;

#ifdef LAZY_SERVICE
const bool kLazyService = true;
#else
const bool kLazyService = false;
#endif

int main() {
  ALOGI("Google camera provider service is starting.");
  // The camera HAL may communicate to other vendor components via
  // /dev/vndbinder
  mallopt(M_DECAY_TIME, 1);
  android::ProcessState::initWithDriver("/dev/vndbinder");
  android::hardware::configureRpcThreadpool(/*maxThreads=*/6,
                                            /*callerWillJoin=*/true);

  android::sp<ICameraProvider> camera_provider = HidlCameraProvider::Create();
  if (camera_provider == nullptr) {
    return android::NO_INIT;
  }
  if (kLazyService) {
    android::hardware::LazyServiceRegistrar& lazy_registrar =
        android::hardware::LazyServiceRegistrar::getInstance();
    if (lazy_registrar.registerService(camera_provider, "internal/0") !=
        android::OK) {
      ALOGE("Cannot register Google camera provider lazy service");
      return android::NO_INIT;
    }
  } else {
    if (camera_provider->registerAsService("internal/0") != android::OK) {
      ALOGE("Cannot register Google camera provider service");
      return android::NO_INIT;
    }
  }
  android::hardware::joinRpcThreadpool();

  // In normal operation, the threadpool should never return.
  return EXIT_FAILURE;
}
