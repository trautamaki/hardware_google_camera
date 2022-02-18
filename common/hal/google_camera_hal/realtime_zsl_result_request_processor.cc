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
#define LOG_TAG "GCH_RealtimeZslResultRequestProcessor"
#define ATRACE_TAG ATRACE_TAG_CAMERA

#include "realtime_zsl_result_request_processor.h"

#include <inttypes.h>
#include <log/log.h>
#include <utils/Trace.h>

#include "hal_utils.h"

namespace android {
namespace google_camera_hal {

std::unique_ptr<RealtimeZslResultRequestProcessor>
RealtimeZslResultRequestProcessor::Create(
    InternalStreamManager* internal_stream_manager, int32_t stream_id,
    android_pixel_format_t pixel_format, uint32_t partial_result_count) {
  ATRACE_CALL();
  if (internal_stream_manager == nullptr) {
    ALOGE("%s: internal_stream_manager is nullptr.", __FUNCTION__);
    return nullptr;
  }

  auto result_processor = std::unique_ptr<RealtimeZslResultRequestProcessor>(
      new RealtimeZslResultRequestProcessor(internal_stream_manager, stream_id,
                                            pixel_format, partial_result_count));
  if (result_processor == nullptr) {
    ALOGE("%s: Creating RealtimeZslResultRequestProcessor failed.",
          __FUNCTION__);
    return nullptr;
  }

  return result_processor;
}

RealtimeZslResultRequestProcessor::RealtimeZslResultRequestProcessor(
    InternalStreamManager* internal_stream_manager, int32_t stream_id,
    android_pixel_format_t pixel_format, uint32_t partial_result_count) {
  internal_stream_manager_ = internal_stream_manager;
  stream_id_ = stream_id;
  pixel_format_ = pixel_format;
  partial_result_count_ = partial_result_count;
}

void RealtimeZslResultRequestProcessor::SetResultCallback(
    ProcessCaptureResultFunc process_capture_result, NotifyFunc notify) {
  std::lock_guard<std::mutex> lock(callback_lock_);
  process_capture_result_ = process_capture_result;
  notify_ = notify;
}

void RealtimeZslResultRequestProcessor::SaveLsForHdrplus(
    const CaptureRequest& request) {
  if (request.settings != nullptr) {
    uint8_t lens_shading_map_mode;
    status_t res =
        hal_utils::GetLensShadingMapMode(request, &lens_shading_map_mode);
    if (res == OK) {
      current_lens_shading_map_mode_ = lens_shading_map_mode;
    }
  }

  {
    std::lock_guard<std::mutex> lock(lens_shading_lock_);
    requested_lens_shading_map_modes_.emplace(request.frame_number,
                                              current_lens_shading_map_mode_);
  }
}

status_t RealtimeZslResultRequestProcessor::HandleLsResultForHdrplus(
    uint32_t frameNumber, HalCameraMetadata* metadata) {
  if (metadata == nullptr) {
    ALOGE("%s: metadata is nullptr", __FUNCTION__);
    return BAD_VALUE;
  }
  std::lock_guard<std::mutex> lock(lens_shading_lock_);
  auto iter = requested_lens_shading_map_modes_.find(frameNumber);
  if (iter == requested_lens_shading_map_modes_.end()) {
    ALOGW("%s: can't find frame (%d)", __FUNCTION__, frameNumber);
    return OK;
  }

  if (iter->second == ANDROID_STATISTICS_LENS_SHADING_MAP_MODE_OFF) {
    status_t res = hal_utils::RemoveLsInfoFromResult(metadata);
    if (res != OK) {
      ALOGW("%s: RemoveLsInfoFromResult fail", __FUNCTION__);
    }
  }
  requested_lens_shading_map_modes_.erase(iter);

  return OK;
}

void RealtimeZslResultRequestProcessor::SaveFdForHdrplus(
    const CaptureRequest& request) {
  // Enable face detect mode for internal use
  if (request.settings != nullptr) {
    uint8_t fd_mode;
    status_t res = hal_utils::GetFdMode(request, &fd_mode);
    if (res == OK) {
      current_face_detect_mode_ = fd_mode;
    }
  }

  {
    std::lock_guard<std::mutex> lock(face_detect_lock_);
    requested_face_detect_modes_.emplace(request.frame_number,
                                         current_face_detect_mode_);
  }
}

status_t RealtimeZslResultRequestProcessor::HandleFdResultForHdrplus(
    uint32_t frameNumber, HalCameraMetadata* metadata) {
  if (metadata == nullptr) {
    ALOGE("%s: metadata is nullptr", __FUNCTION__);
    return BAD_VALUE;
  }
  std::lock_guard<std::mutex> lock(face_detect_lock_);
  auto iter = requested_face_detect_modes_.find(frameNumber);
  if (iter == requested_face_detect_modes_.end()) {
    ALOGW("%s: can't find frame (%d)", __FUNCTION__, frameNumber);
    return OK;
  }

  if (iter->second == ANDROID_STATISTICS_FACE_DETECT_MODE_OFF) {
    status_t res = hal_utils::RemoveFdInfoFromResult(metadata);
    if (res != OK) {
      ALOGW("%s: RestoreFdMetadataForHdrplus fail", __FUNCTION__);
    }
  }
  requested_face_detect_modes_.erase(iter);

  return OK;
}

status_t RealtimeZslResultRequestProcessor::AddPendingRequests(
    const std::vector<ProcessBlockRequest>& process_block_requests,
    const CaptureRequest& remaining_session_request) {
  ATRACE_CALL();
  // This is the last result processor. Sanity check if requests contains
  // all remaining output buffers.
  if (!hal_utils::AreAllRemainingBuffersRequested(process_block_requests,
                                                  remaining_session_request)) {
    ALOGE("%s: Some output buffers will not be completed.", __FUNCTION__);
    return BAD_VALUE;
  }

  if (pixel_format_ == HAL_PIXEL_FORMAT_RAW10) {
    SaveFdForHdrplus(remaining_session_request);
    SaveLsForHdrplus(remaining_session_request);
  }

  return OK;
}

void RealtimeZslResultRequestProcessor::ProcessResult(
    ProcessBlockResult block_result) {
  ATRACE_CALL();
  std::lock_guard<std::mutex> lock(callback_lock_);
  std::unique_ptr<CaptureResult> result = std::move(block_result.result);
  if (result == nullptr) {
    ALOGW("%s: Received a nullptr result.", __FUNCTION__);
    return;
  }

  if (process_capture_result_ == nullptr) {
    ALOGE("%s: process_capture_result_ is nullptr. Dropping a result.",
          __FUNCTION__);
    return;
  }

  // Return filled raw buffer to internal stream manager
  // And remove raw buffer from result
  bool returned_output = false;
  status_t res;
  std::vector<StreamBuffer> modified_output_buffers;
  for (uint32_t i = 0; i < result->output_buffers.size(); i++) {
    if (stream_id_ == result->output_buffers[i].stream_id) {
      returned_output = true;
      res = internal_stream_manager_->ReturnFilledBuffer(
          result->frame_number, result->output_buffers[i]);
      if (res != OK) {
        ALOGW("%s: (%d)ReturnStreamBuffer fail", __FUNCTION__,
              result->frame_number);
      }
    } else {
      modified_output_buffers.push_back(result->output_buffers[i]);
    }
  }

  if (result->output_buffers.size() > 0) {
    result->output_buffers.clear();
    result->output_buffers = modified_output_buffers;
  }

  if (result->result_metadata) {
    result->result_metadata->Erase(ANDROID_CONTROL_ENABLE_ZSL);

    res = internal_stream_manager_->ReturnMetadata(
        stream_id_, result->frame_number, result->result_metadata.get(),
        result->partial_result);
    if (res != OK) {
      ALOGW("%s: (%d)ReturnMetadata fail", __FUNCTION__, result->frame_number);
    }

    if (result->partial_result == partial_result_count_) {
      res =
          hal_utils::SetEnableZslMetadata(result->result_metadata.get(), false);
      if (res != OK) {
        ALOGW("%s: SetEnableZslMetadata (%d) fail", __FUNCTION__,
              result->frame_number);
      }

      if (pixel_format_ == HAL_PIXEL_FORMAT_RAW10) {
        res = HandleFdResultForHdrplus(result->frame_number,
                                       result->result_metadata.get());
        if (res != OK) {
          ALOGE("%s: HandleFdResultForHdrplus(%d) fail", __FUNCTION__,
                result->frame_number);
          return;
        }

        res = HandleLsResultForHdrplus(result->frame_number,
                                       result->result_metadata.get());
        if (res != OK) {
          ALOGE("%s: HandleLsResultForHdrplus(%d) fail", __FUNCTION__,
                result->frame_number);
          return;
        }
      }
    }
  }

  // Don't send result to framework if only internal raw callback
  if (returned_output && result->result_metadata == nullptr &&
      result->output_buffers.size() == 0) {
    return;
  }
  process_capture_result_(std::move(result));
}

void RealtimeZslResultRequestProcessor::Notify(
    const ProcessBlockNotifyMessage& block_message) {
  ATRACE_CALL();
  std::lock_guard<std::mutex> lock(callback_lock_);
  const NotifyMessage& message = block_message.message;
  if (notify_ == nullptr) {
    ALOGE("%s: notify_ is nullptr. Dropping a message.", __FUNCTION__);
    return;
  }

  notify_(message);
}

status_t RealtimeZslResultRequestProcessor::FlushPendingRequests() {
  ATRACE_CALL();
  return INVALID_OPERATION;
}

status_t RealtimeZslResultRequestProcessor::ConfigureStreams(
    InternalStreamManager* /*internal_stream_manager*/,
    const StreamConfiguration& stream_config,
    StreamConfiguration* process_block_stream_config) {
  ATRACE_CALL();
  if (process_block_stream_config == nullptr) {
    ALOGE("%s: process_block_stream_config is nullptr", __FUNCTION__);
    return BAD_VALUE;
  }

  process_block_stream_config->streams = stream_config.streams;
  process_block_stream_config->operation_mode = stream_config.operation_mode;
  process_block_stream_config->session_params =
      HalCameraMetadata::Clone(stream_config.session_params.get());
  process_block_stream_config->stream_config_counter =
      stream_config.stream_config_counter;
  process_block_stream_config->multi_resolution_input_image =
      stream_config.multi_resolution_input_image;

  return OK;
}

status_t RealtimeZslResultRequestProcessor::SetProcessBlock(
    std::unique_ptr<ProcessBlock> process_block) {
  ATRACE_CALL();
  if (process_block == nullptr) {
    ALOGE("%s: process_block is nullptr", __FUNCTION__);
    return BAD_VALUE;
  }

  std::lock_guard lock(process_block_shared_lock_);
  if (process_block_ != nullptr) {
    ALOGE("%s: Already configured.", __FUNCTION__);
    return ALREADY_EXISTS;
  }

  process_block_ = std::move(process_block);
  return OK;
}

status_t RealtimeZslResultRequestProcessor::ProcessRequest(
    const CaptureRequest& request) {
  ATRACE_CALL();
  std::shared_lock lock(process_block_shared_lock_);
  if (process_block_ == nullptr) {
    ALOGE("%s: Not configured yet.", __FUNCTION__);
    return NO_INIT;
  }

  CaptureRequest block_request;
  block_request.frame_number = request.frame_number;
  block_request.settings = HalCameraMetadata::Clone(request.settings.get());
  block_request.input_buffers = request.input_buffers;
  block_request.input_width = request.input_width;
  block_request.input_height = request.input_height;

  for (auto& metadata : request.input_buffer_metadata) {
    block_request.input_buffer_metadata.push_back(
        HalCameraMetadata::Clone(metadata.get()));
  }

  block_request.output_buffers = request.output_buffers;
  for (auto& [camera_id, physical_metadata] : request.physical_camera_settings) {
    block_request.physical_camera_settings[camera_id] =
        HalCameraMetadata::Clone(physical_metadata.get());
  }

  std::vector<ProcessBlockRequest> block_requests(1);
  block_requests[0].request = std::move(block_request);

  return process_block_->ProcessRequests(block_requests, request);
}

status_t RealtimeZslResultRequestProcessor::Flush() {
  ATRACE_CALL();
  std::shared_lock lock(process_block_shared_lock_);
  if (process_block_ == nullptr) {
    return OK;
  }

  return process_block_->Flush();
}

}  // namespace google_camera_hal
}  // namespace android