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

#ifndef HARDWARE_GOOGLE_CAMERA_HAL_GOOGLE_CAMERA_HAL_REALTIME_ZSL_RESULT_REQUEST_PROCESSOR_H_
#define HARDWARE_GOOGLE_CAMERA_HAL_GOOGLE_CAMERA_HAL_REALTIME_ZSL_RESULT_REQUEST_PROCESSOR_H_

#include <shared_mutex>

#include "internal_stream_manager.h"
#include "realtime_zsl_result_processor.h"
#include "request_processor.h"
#include "result_processor.h"

namespace android {
namespace google_camera_hal {

// RealtimeZslResultRequestProcessor implements a RealtimeZslResultProcessor
// that return filled raw buffer and metadata to internal stream manager. It
// also implements a RequestProcess to forward the results.
class RealtimeZslResultRequestProcessor : public RealtimeZslResultProcessor,
                                          RequestProcessor {
 public:
  static std::unique_ptr<RealtimeZslResultRequestProcessor> Create(
      InternalStreamManager* internal_stream_manager, int32_t stream_id,
      android_pixel_format_t pixel_format, uint32_t partial_result_count = 1);

  virtual ~RealtimeZslResultRequestProcessor() = default;

  // Override functions of RealtimeZslResultProcessor start.
  void ProcessResult(ProcessBlockResult block_result) override;
  // Override functions of RealtimeZslResultProcessor end.

  // Override functions of RequestProcessor start.
  status_t ConfigureStreams(
      InternalStreamManager* internal_stream_manager,
      const StreamConfiguration& stream_config,
      StreamConfiguration* process_block_stream_config) override;

  status_t SetProcessBlock(std::unique_ptr<ProcessBlock> process_block) override;

  status_t ProcessRequest(const CaptureRequest& request) override;

  status_t Flush() override;
  // Override functions of RequestProcessor end.

 protected:
  RealtimeZslResultRequestProcessor(
      InternalStreamManager* internal_stream_manager, int32_t stream_id,
      android_pixel_format_t pixel_format, uint32_t partial_result_count);

 private:
  std::shared_mutex process_block_shared_lock_;

  // Protected by process_block_shared_lock_.
  std::unique_ptr<ProcessBlock> process_block_;
};

}  // namespace google_camera_hal
}  // namespace android

#endif  // HARDWARE_GOOGLE_CAMERA_HAL_GOOGLE_CAMERA_HAL_REALTIME_ZSL_RESULT_REQUEST_PROCESSOR_H_
