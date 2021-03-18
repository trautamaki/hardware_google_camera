/*
 * Copyright (C) 2021 The Android Open Source Project
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

#define LOG_TAG "GCH_ZslSnapshotCaptureSession"
#define ATRACE_TAG ATRACE_TAG_CAMERA
#include "zsl_snapshot_capture_session.h"

#include <log/log.h>
#include <utils/Trace.h>

#include "capture_session_wrapper_process_block.h"
#include "hal_utils.h"
#include "realtime_zsl_request_processor.h"
#include "realtime_zsl_result_processor.h"
#include "system/graphics-base-v1.0.h"
#include "utils.h"
#include "utils/Errors.h"

namespace android {
namespace google_camera_hal {

bool ZslSnapshotCaptureSession::IsStreamConfigurationSupported(
    CameraDeviceSessionHwl* device_session_hwl,
    const StreamConfiguration& stream_config) {
  ATRACE_CALL();
  if (device_session_hwl == nullptr) {
    ALOGE("%s: device_session_hwl is nullptr", __FUNCTION__);
    return false;
  }

  bool has_physical_camera_stream = false;
  bool has_non_physical_camera_stream = false;
  bool has_jpeg_stream = false;
  bool has_preview_stream = false;
  std::set<int> physical_camera_in_use;
  for (const auto& stream : stream_config.streams) {
    if (stream.is_physical_camera_stream) {
      has_physical_camera_stream = true;
      physical_camera_in_use.insert(stream.physical_camera_id);
    } else {
      has_non_physical_camera_stream = true;
    }
    if (utils::IsJPEGSnapshotStream(stream)) {
      has_jpeg_stream = true;
    } else if (utils::IsPreviewStream(stream)) {
      has_preview_stream = true;
    } else if (!utils::IsYUVSnapshotStream(stream)) {
      ALOGE("%s: only support preview + snapshot (+ YUV) streams", __FUNCTION__);
      return false;
    }
  }
  if (!has_jpeg_stream) {
    ALOGE("%s: no JPEG stream", __FUNCTION__);
    return false;
  }

  if (!has_preview_stream) {
    ALOGE("%s: no preview stream", __FUNCTION__);
    return false;
  }

  if (has_physical_camera_stream && has_non_physical_camera_stream) {
    ALOGE("%s: support only physical camera or logical camera, but not both",
          __FUNCTION__);
    return false;
  }

  if (has_physical_camera_stream && physical_camera_in_use.size() != 1) {
    ALOGE("%s: support only 1 physical camera, but %d have set up streams",
          __FUNCTION__, static_cast<int>(physical_camera_in_use.size()));
  }

  ALOGD("%s: ZslSnapshotCaptureSession supports the stream config",
        __FUNCTION__);
  return true;
}

std::unique_ptr<CaptureSession> ZslSnapshotCaptureSession::Create(
    const StreamConfiguration& stream_config,
    const std::vector<ExternalCaptureSessionFactory*>&
        external_capture_session_entries,
    const std::vector<CaptureSessionEntryFuncs>& capture_session_entries,
    HwlSessionCallback hwl_session_callback,
    CameraBufferAllocatorHwl* camera_buffer_allocator_hwl,
    CameraDeviceSessionHwl* camera_device_session_hwl,
    std::vector<HalStream>* hal_configured_streams,
    ProcessCaptureResultFunc process_capture_result, NotifyFunc notify) {
  ATRACE_CALL();
  auto session =
      std::unique_ptr<ZslSnapshotCaptureSession>(new ZslSnapshotCaptureSession(
          external_capture_session_entries, capture_session_entries,
          hwl_session_callback, camera_buffer_allocator_hwl,
          camera_device_session_hwl));
  if (session == nullptr) {
    ALOGE("%s: Creating ZslSnapshotCaptureSession failed.", __FUNCTION__);
    return nullptr;
  }

  status_t res = session->Initialize(camera_device_session_hwl, stream_config,
                                     process_capture_result, notify,
                                     hal_configured_streams);
  if (res != OK) {
    ALOGE("%s: Initializing ZslSnapshotCaptureSession failed: %s (%d).",
          __FUNCTION__, strerror(-res), res);
    return nullptr;
  }
  return session;
}

ZslSnapshotCaptureSession::~ZslSnapshotCaptureSession() {
  if (camera_device_session_hwl_ != nullptr) {
    camera_device_session_hwl_->DestroyPipelines();
  }
}

status_t ZslSnapshotCaptureSession::BuildPipelines(
    ProcessBlock* process_block, ProcessBlock* /*snapshot_process_block*/,
    std::vector<HalStream>* hal_configured_streams) {
  ATRACE_CALL();
  if (process_block == nullptr || hal_configured_streams == nullptr) {
    ALOGE("%s: process_block (%p) or hal_configured_streams (%p) is nullptr",
          __FUNCTION__, process_block, hal_configured_streams);
    return BAD_VALUE;
  }

  status_t res;
  for (uint32_t i = 0; i < hal_configured_streams->size(); i++) {
    if (hal_configured_streams->at(i).id == additional_stream_id_) {
      if (hal_configured_streams->at(i).max_buffers < kRawMinBufferCount) {
        hal_configured_streams->at(i).max_buffers = kRawMinBufferCount;
      }
      // Allocate internal raw stream buffers
      uint32_t additional_num_buffers =
          (hal_configured_streams->at(i).max_buffers >= kRawBufferCount)
              ? 0
              : (kRawBufferCount - hal_configured_streams->at(i).max_buffers);
      res = internal_stream_manager_->AllocateBuffers(
          hal_configured_streams->at(i), additional_num_buffers);
      if (res != OK) {
        ALOGE("%s: AllocateBuffers failed.", __FUNCTION__);
        return UNKNOWN_ERROR;
      }
      break;
    }
  }

  return OK;
}

status_t ZslSnapshotCaptureSession::ConfigureStreams(
    const StreamConfiguration& stream_config,
    RequestProcessor* request_processor, ProcessBlock* process_block,
    int32_t& additional_stream_id) {
  ATRACE_CALL();
  if (request_processor == nullptr || process_block == nullptr) {
    ALOGE("%s: request_processor (%p) or process_block (%p) is nullptr",
          __FUNCTION__, request_processor, process_block);
    return BAD_VALUE;
  }
  StreamConfiguration process_block_stream_config;
  // Configure streams for request processor
  status_t res = request_processor->ConfigureStreams(
      internal_stream_manager_.get(), stream_config,
      &process_block_stream_config);
  if (res != OK) {
    ALOGE("%s: Configuring stream for request processor failed.", __FUNCTION__);
    return res;
  }

  // Check all streams are configured.
  if (stream_config.streams.size() > process_block_stream_config.streams.size()) {
    ALOGE("%s: stream_config has %zu streams but only configured %zu streams",
          __FUNCTION__, stream_config.streams.size(),
          process_block_stream_config.streams.size());
    return UNKNOWN_ERROR;
  }
  for (auto& stream : stream_config.streams) {
    bool found = false;
    for (auto& configured_stream : process_block_stream_config.streams) {
      if (stream.id == configured_stream.id) {
        found = true;
        break;
      }
    }

    if (!found) {
      ALOGE("%s: Cannot find stream %u in configured streams.", __FUNCTION__,
            stream.id);
      return UNKNOWN_ERROR;
    }
  }

  for (auto& stream : process_block_stream_config.streams) {
    bool found = false;
    for (auto& configured_stream : stream_config.streams) {
      if (stream.id == configured_stream.id) {
        found = true;
        break;
      }
    }
    if (!found) {
      additional_stream_id = stream.id;
      break;
    }
  }

  if (additional_stream_id == -1) {
    ALOGE("%s: Configuring stream fail due to wrong additional_stream_id",
          __FUNCTION__);
    return UNKNOWN_ERROR;
  }

  // Configure streams for process block.
  // TODO(mhtan): use process_block_stream_config for the first parameter later.
  res = process_block->ConfigureStreams(stream_config, stream_config);
  if (res != OK) {
    ALOGE("%s: Configuring stream for process block failed.", __FUNCTION__);
    return res;
  }

  return OK;
}

status_t ZslSnapshotCaptureSession::SetupPreviewProcessChain(
    const StreamConfiguration& stream_config,
    ProcessCaptureResultFunc process_capture_result, NotifyFunc notify,
    int32_t& stream_id) {
  ATRACE_CALL();
  if (preview_process_block_ != nullptr ||
      preview_result_processor_ != nullptr ||
      preview_request_processor_ != nullptr) {
    ALOGE(
        "%s: preview_process_block_(%p) or preview_result_processor_(%p) or "
        "preview_request_processor_(%p) is/are "
        "already set",
        __FUNCTION__, preview_process_block_, preview_result_processor_,
        preview_request_processor_.get());
    return BAD_VALUE;
  }

  auto preview_process_block = CaptureSessionWrapperProcessBlock::Create(
      external_capture_session_entries_, capture_session_entries_,
      hwl_session_callback_, camera_buffer_allocator_hwl_,
      camera_device_session_hwl_, hal_config_);
  if (preview_process_block == nullptr) {
    ALOGE("%s: Creating RealtimeProcessBlock failed.", __FUNCTION__);
    return UNKNOWN_ERROR;
  }
  preview_process_block_ = preview_process_block.get();

  // Create preview request processor.
  preview_request_processor_ = RealtimeZslRequestProcessor::Create(
      camera_device_session_hwl_, HAL_PIXEL_FORMAT_YCBCR_420_888);
  if (preview_request_processor_ == nullptr) {
    ALOGE("%s: Creating RealtimeZslRequestProcessor failed.", __FUNCTION__);
    return UNKNOWN_ERROR;
  }

  // Create preview result processor.
  auto preview_result_processor = RealtimeZslResultProcessor::Create(
      internal_stream_manager_.get(), stream_id, HAL_PIXEL_FORMAT_YCBCR_420_888);
  if (preview_result_processor == nullptr) {
    ALOGE("%s: Creating PreviewZslResultProcessor failed.", __FUNCTION__);
    return UNKNOWN_ERROR;
  }
  preview_result_processor_ = preview_result_processor.get();
  preview_result_processor->SetResultCallback(process_capture_result, notify);

  status_t res = preview_process_block->SetResultProcessor(
      std::move(preview_result_processor));
  if (res != OK) {
    ALOGE("%s: Setting result process in process block failed.", __FUNCTION__);
    return res;
  }

  res = preview_request_processor_->SetProcessBlock(
      std::move(preview_process_block));
  if (res != OK) {
    ALOGE("%s: Setting process block for RequestProcessor failed: %s(%d)",
          __FUNCTION__, strerror(-res), res);
    return res;
  }

  res = ConfigureStreams(stream_config, preview_request_processor_.get(),
                         preview_process_block_, stream_id);
  if (res != OK) {
    ALOGE("%s: Configuring stream failed: %s(%d)", __FUNCTION__, strerror(-res),
          res);
    return res;
  }
  return OK;
}

status_t ZslSnapshotCaptureSession::PurgeHalConfiguredStream(
    const StreamConfiguration& stream_config,
    std::vector<HalStream>* hal_configured_streams) {
  if (hal_configured_streams == nullptr) {
    ALOGE("%s: HAL configured stream list is null.", __FUNCTION__);
    return BAD_VALUE;
  }

  std::set<int32_t> framework_stream_id_set;
  for (auto& stream : stream_config.streams) {
    framework_stream_id_set.insert(stream.id);
  }

  std::vector<HalStream> configured_streams;
  for (auto& hal_stream : *hal_configured_streams) {
    if (framework_stream_id_set.find(hal_stream.id) !=
        framework_stream_id_set.end()) {
      configured_streams.push_back(hal_stream);
    }
  }
  *hal_configured_streams = configured_streams;
  return OK;
}

ZslSnapshotCaptureSession::ZslSnapshotCaptureSession(
    const std::vector<ExternalCaptureSessionFactory*>&
        external_capture_session_entries,
    const std::vector<CaptureSessionEntryFuncs>& capture_session_entries,
    HwlSessionCallback hwl_session_callback,
    CameraBufferAllocatorHwl* camera_buffer_allocator_hwl,
    CameraDeviceSessionHwl* camera_device_session_hwl)
    : external_capture_session_entries_(external_capture_session_entries),
      capture_session_entries_(capture_session_entries),
      hwl_session_callback_(hwl_session_callback),
      camera_buffer_allocator_hwl_(camera_buffer_allocator_hwl),
      camera_device_session_hwl_(camera_device_session_hwl) {
}

status_t ZslSnapshotCaptureSession::Initialize(
    CameraDeviceSessionHwl* camera_device_session_hwl,
    const StreamConfiguration& stream_config,
    ProcessCaptureResultFunc process_capture_result, NotifyFunc notify,
    std::vector<HalStream>* hal_configured_streams) {
  ATRACE_CALL();
  if (!IsStreamConfigurationSupported(camera_device_session_hwl, stream_config)) {
    ALOGE("%s: stream configuration is not supported.", __FUNCTION__);
    return BAD_VALUE;
  }

  std::unique_ptr<HalCameraMetadata> characteristics;
  status_t res =
      camera_device_session_hwl->GetCameraCharacteristics(&characteristics);
  if (res != OK) {
    ALOGE("%s: GetCameraCharacteristics failed.", __FUNCTION__);
    return BAD_VALUE;
  }

  for (auto stream : stream_config.streams) {
    if (utils::IsPreviewStream(stream)) {
      hal_preview_stream_id_ = stream.id;
      break;
    }
  }
  camera_device_session_hwl_ = camera_device_session_hwl;
  hal_config_ = hal_configured_streams;
  internal_stream_manager_ = InternalStreamManager::Create();
  if (internal_stream_manager_ == nullptr) {
    ALOGE("%s: Cannot create internal stream manager.", __FUNCTION__);
    return UNKNOWN_ERROR;
  }

  // Create result dispatcher
  result_dispatcher_ =
      ResultDispatcher::Create(kPartialResult, process_capture_result, notify);
  if (result_dispatcher_ == nullptr) {
    ALOGE("%s: Cannot create result dispatcher.", __FUNCTION__);
    return UNKNOWN_ERROR;
  }

  device_session_notify_ = notify;
  process_capture_result_ =
      ProcessCaptureResultFunc([this](std::unique_ptr<CaptureResult> result) {
        ProcessCaptureResult(std::move(result));
      });
  notify_ = NotifyFunc(
      [this](const NotifyMessage& message) { NotifyHalMessage(message); });

  // Setup and connect realtime process chain
  res = SetupPreviewProcessChain(stream_config, process_capture_result_,
                                 notify_, additional_stream_id_);
  if (res != OK) {
    ALOGE("%s: SetupRealtimeProcessChain fail: %s(%d)", __FUNCTION__,
          strerror(-res), res);
    return res;
  }

  // Setup snapshot process chain
  std::unique_ptr<ProcessBlock> snapshot_process_block;
  std::unique_ptr<ResultProcessor> snapshot_result_processor;

  // Realtime and snapshot streams are configured
  // Start to build pipleline
  res = BuildPipelines(preview_process_block_, snapshot_process_block.get(),
                       hal_configured_streams);
  if (res != OK) {
    ALOGE("%s: Building pipelines failed: %s(%d)", __FUNCTION__, strerror(-res),
          res);
    return res;
  }

  res = PurgeHalConfiguredStream(stream_config, hal_configured_streams);
  if (res != OK) {
    ALOGE("%s: Removing internal streams from configured stream failed: %s(%d)",
          __FUNCTION__, strerror(-res), res);
    return res;
  }

  if (res != OK) {
    ALOGE("%s: Connecting process chain failed: %s(%d)", __FUNCTION__,
          strerror(-res), res);
    return res;
  }

  return OK;
}

status_t ZslSnapshotCaptureSession::ProcessRequest(const CaptureRequest& request) {
  ATRACE_CALL();
  status_t res = result_dispatcher_->AddPendingRequest(request);
  if (res != OK) {
    ALOGE("%s: frame(%d) fail to AddPendingRequest", __FUNCTION__,
          request.frame_number);
    return BAD_VALUE;
  }
  res = preview_request_processor_->ProcessRequest(request);

  if (res != OK) {
    ALOGE("%s: ProcessRequest (%d) fail and remove pending request",
          __FUNCTION__, request.frame_number);
    result_dispatcher_->RemovePendingRequest(request.frame_number);
  }
  return res;
}

status_t ZslSnapshotCaptureSession::Flush() {
  ATRACE_CALL();
  return preview_request_processor_->Flush();
}

void ZslSnapshotCaptureSession::ProcessCaptureResult(
    std::unique_ptr<CaptureResult> result) {
  ATRACE_CALL();
  std::lock_guard<std::mutex> lock(callback_lock_);
  if (result == nullptr) {
    return;
  }

  if (result->result_metadata) {
    camera_device_session_hwl_->FilterResultMetadata(
        result->result_metadata.get());
  }

  status_t res = result_dispatcher_->AddResult(std::move(result));
  if (res != OK) {
    ALOGE("%s: fail to AddResult", __FUNCTION__);
    return;
  }
}

void ZslSnapshotCaptureSession::NotifyHalMessage(const NotifyMessage& message) {
  ATRACE_CALL();
  std::lock_guard<std::mutex> lock(callback_lock_);
  if (device_session_notify_ == nullptr) {
    ALOGE("%s: device_session_notify_ is nullptr. Dropping a message.",
          __FUNCTION__);
    return;
  }

  if (message.type == MessageType::kShutter) {
    status_t res =
        result_dispatcher_->AddShutter(message.message.shutter.frame_number,
                                       message.message.shutter.timestamp_ns);
    if (res != OK) {
      ALOGE("%s: AddShutter for frame %u failed: %s (%d).", __FUNCTION__,
            message.message.shutter.frame_number, strerror(-res), res);
      return;
    }
  } else if (message.type == MessageType::kError) {
    status_t res = result_dispatcher_->AddError(message.message.error);
    if (res != OK) {
      ALOGE("%s: AddError for frame %u failed: %s (%d).", __FUNCTION__,
            message.message.error.frame_number, strerror(-res), res);
      return;
    }
  } else {
    ALOGW("%s: Unsupported message type: %u", __FUNCTION__, message.type);
    device_session_notify_(message);
  }
}

}  // namespace google_camera_hal
}  // namespace android