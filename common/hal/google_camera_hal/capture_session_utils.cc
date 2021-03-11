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

#include "capture_session_utils.h"

namespace android {
namespace google_camera_hal {

std::unique_ptr<CaptureSession> CreateCaptureSession(
    const StreamConfiguration& stream_config,
    const std::vector<ExternalCaptureSessionFactory*>&
        external_capture_session_entries,
    const std::vector<CaptureSessionEntryFuncs>& capture_session_entries,
    HwlSessionCallback hwl_session_callback,
    CameraBufferAllocatorHwl* camera_buffer_allocator_hwl,
    CameraDeviceSessionHwl* camera_device_session_hwl,
    std::vector<HalStream>* hal_config,
    ProcessCaptureResultFunc process_capture_result, NotifyFunc notify,
    bool /*consider_zsl_capture_session*/) {
  // TODO(mhtan): Add ZslCaptureSession selector when it is supported.

  // first pass: check loaded external capture sessions
  for (auto externalSession : external_capture_session_entries) {
    if (externalSession->IsStreamConfigurationSupported(
            camera_device_session_hwl, stream_config)) {
      return externalSession->CreateSession(
          camera_device_session_hwl, stream_config, process_capture_result,
          notify, hwl_session_callback, hal_config, camera_buffer_allocator_hwl);
    }
  }

  // second pass: check predefined capture sessions
  for (auto sessionEntry : capture_session_entries) {
    if (sessionEntry.IsStreamConfigurationSupported(camera_device_session_hwl,
                                                    stream_config)) {
      return sessionEntry.CreateSession(
          camera_device_session_hwl, stream_config, process_capture_result,
          notify, hwl_session_callback, hal_config, camera_buffer_allocator_hwl);
    }
  }
  return nullptr;
}

}  // namespace google_camera_hal
}  // namespace android