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
#define LOG_TAG "JpegCompressor"

#include <camera3.h>
#include <cutils/properties.h>
#include <libyuv.h>
#include <utils/Log.h>
#include <utils/Trace.h>

#include "JpegCompressor.h"

namespace android {

using google_camera_hal::ErrorCode;
using google_camera_hal::NotifyMessage;
using google_camera_hal::MessageType;

JpegCompressor::JpegCompressor(std::unique_ptr<ExifUtils> exifUtils) :
    mExifUtils(std::move(exifUtils)) {

    ATRACE_CALL();
    mJpegProcessingThread = std::thread([this] { this->threadLoop(); });
}

JpegCompressor::~JpegCompressor() {
    ATRACE_CALL();

    // Abort the ongoing compression and flush any pending jobs
    mJpegDone = true;
    mCondition.notify_one();
    mJpegProcessingThread.join();
    while (!mPendingYUVJobs.empty()) {
        auto job = std::move(mPendingYUVJobs.front());
        job->output->streamBuffer.status = BufferStatus::kError;
        mPendingYUVJobs.pop();
    }
}

status_t JpegCompressor::queueYUV420(std::unique_ptr<JpegYUV420Job> job) {
    ATRACE_CALL();

    if ((job->input.get() == nullptr) || (job->output.get() == nullptr) ||
            (job->output->format != HAL_PIXEL_FORMAT_BLOB) ||
            (job->output->dataSpace != HAL_DATASPACE_V0_JFIF)) {
        ALOGE("%s: Unable to find buffers for JPEG source/destination", __FUNCTION__);
        return BAD_VALUE;
    }

    std::unique_lock<std::mutex> lock(mMutex);
    mPendingYUVJobs.push(std::move(job));
    mCondition.notify_one();

    return OK;
}

void JpegCompressor::threadLoop() {
    ATRACE_CALL();

    while (!mJpegDone) {
        std::unique_ptr<JpegYUV420Job> currentYUVJob = nullptr;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (!mPendingYUVJobs.empty()) {
                currentYUVJob = std::move(mPendingYUVJobs.front());
                mPendingYUVJobs.pop();
            }
        }

        if (currentYUVJob.get() != nullptr) {
            compressYUV420(std::move(currentYUVJob));
        }

        std::unique_lock<std::mutex> lock(mMutex);
        auto ret = mCondition.wait_for(lock, std::chrono::milliseconds(10));
        if (ret == std::cv_status::timeout) {
            ALOGV("%s: Jpeg thread timeout", __FUNCTION__);
        }
    }
}

void JpegCompressor::compressYUV420(std::unique_ptr<JpegYUV420Job> job) {
    const uint8_t *app1Buffer = nullptr;
    size_t app1BufferSize = 0;
    std::vector<uint8_t> thumbnailJpegBuffer;
    size_t encodedThumbnailSize = 0;
    if ((mExifUtils.get() != nullptr) && (job->resultMetadata.get() != nullptr)) {
        if (mExifUtils->initialize()) {
            camera_metadata_ro_entry_t entry;
            size_t thumbnailWidth = 0;
            size_t thumbnailHeight = 0;
            std::vector<uint8_t> thumbYUV420Frame;
            YCbCrPlanes thumbPlanes;
            auto ret = job->resultMetadata->Get(ANDROID_JPEG_THUMBNAIL_SIZE, &entry);
            if ((ret == OK) && (entry.count == 2)) {
                thumbnailWidth = entry.data.i32[0];
                thumbnailHeight = entry.data.i32[1];
                if ((thumbnailWidth > 0) && (thumbnailHeight > 0)) {
                    thumbYUV420Frame.resize((thumbnailWidth * thumbnailHeight * 3) / 2);
                    thumbPlanes = {
                            .imgY = thumbYUV420Frame.data(),
                            .imgCb = thumbYUV420Frame.data() + thumbnailWidth * thumbnailHeight,
                            .imgCr = thumbYUV420Frame.data() +
                                    (thumbnailWidth * thumbnailHeight * 5) / 4,
                            .yStride = thumbnailWidth,
                            .CbCrStride = thumbnailWidth / 2};
                    // TODO: Crop thumbnail according to documentation
                    auto stat = I420Scale(
                            job->input->yuvPlanes.imgY,
                            job->input->yuvPlanes.yStride,
                            job->input->yuvPlanes.imgCb,
                            job->input->yuvPlanes.CbCrStride,
                            job->input->yuvPlanes.imgCr,
                            job->input->yuvPlanes.CbCrStride,
                            job->input->width,
                            job->input->height,
                            thumbPlanes.imgY,
                            thumbPlanes.yStride,
                            thumbPlanes.imgCb,
                            thumbPlanes.CbCrStride,
                            thumbPlanes.imgCr,
                            thumbPlanes.CbCrStride,
                            thumbnailWidth,
                            thumbnailHeight,
                            libyuv::kFilterNone);
                    if (stat != 0) {
                        ALOGE("%s: Failed during thumbnail scaling: %d", __FUNCTION__, stat);
                        thumbYUV420Frame.clear();
                    }
                }
            }

            if (mExifUtils->setFromMetadata(*job->resultMetadata, job->input->width,
                        job->input->height)) {
                if (!thumbYUV420Frame.empty()) {
                    thumbnailJpegBuffer.resize(64*1024); //APP1 is limited by 64k
                    encodedThumbnailSize = compressYUV420Frame({
                            .outputBuffer = thumbnailJpegBuffer.data(),
                            .outputBufferSize = thumbnailJpegBuffer.size(),
                            .yuvPlanes = thumbPlanes,
                            .width = thumbnailWidth,
                            .height = thumbnailHeight,
                            .app1Buffer = nullptr,
                            .app1BufferSize = 0});
                    if (encodedThumbnailSize > 0) {
                        job->output->streamBuffer.status = BufferStatus::kOk;
                    } else {
                        ALOGE("%s: Failed encoding thumbail!", __FUNCTION__);
                        thumbnailJpegBuffer.clear();
                    }
                }

                char value[PROPERTY_VALUE_MAX];
                if (property_get("ro.product.vendor.manufacturer", value, "unknown") > 0) {
                    mExifUtils->setMake(std::string(value));
                } else {
                    ALOGW("%s: No Exif make data!", __FUNCTION__);
                }

                if (property_get("ro.product.vendor.model", value, "unknown") > 0) {
                    mExifUtils->setModel(std::string(value));
                } else {
                    ALOGW("%s: No Exif model data!", __FUNCTION__);
                }

                if (mExifUtils->generateApp1(thumbnailJpegBuffer.empty() ?
                            nullptr : thumbnailJpegBuffer.data(), encodedThumbnailSize)) {
                    app1Buffer = mExifUtils->getApp1Buffer();
                    app1BufferSize = mExifUtils->getApp1Length();
                } else {
                    ALOGE("%s: Unable to generate App1 buffer", __FUNCTION__);
                }
            } else {
                ALOGE("%s: Unable to generate EXIF section!", __FUNCTION__);
            }
        } else {
            ALOGE("%s: Unable to initialize Exif generator!", __FUNCTION__);
        }
    }

    auto encodedSize = compressYUV420Frame({
            .outputBuffer = job->output->plane.img.img,
            .outputBufferSize = job->output->plane.img.bufferSize,
            .yuvPlanes = job->input->yuvPlanes,
            .width = job->input->width,
            .height = job->input->height,
            .app1Buffer = app1Buffer,
            .app1BufferSize = app1BufferSize});
    if (encodedSize > 0) {
        job->output->streamBuffer.status = BufferStatus::kOk;
    } else {
        job->output->streamBuffer.status = BufferStatus::kError;
        return;
    }

    auto jpegHeaderOffset = job->output->plane.img.bufferSize - sizeof(struct camera3_jpeg_blob);
    if (jpegHeaderOffset > encodedSize) {
        struct camera3_jpeg_blob *blob = reinterpret_cast<struct camera3_jpeg_blob*> (
                job->output->plane.img.img + jpegHeaderOffset);
        blob->jpeg_blob_id = CAMERA3_JPEG_BLOB_ID;
        blob->jpeg_size = encodedSize;
    } else {
        ALOGW("%s: No space for jpeg header at offset: %u and jpeg size: %u", __FUNCTION__,
                jpegHeaderOffset, encodedSize);
    }
}

size_t JpegCompressor::compressYUV420Frame(YUV420Frame frame) {
    ATRACE_CALL();

    struct CustomJpegDestMgr : public jpeg_destination_mgr {
        JOCTET *buffer;
        size_t bufferSize;
        size_t encodedSize;
        bool success;
    } dmgr;

    // Set up error management
    mJpegErrorInfo = NULL;
    jpeg_error_mgr jerr;

    auto cinfo = std::make_unique<jpeg_compress_struct>();
    cinfo->err = jpeg_std_error(&jerr);
    cinfo->err->error_exit = [](j_common_ptr cinfo) {
        (*cinfo->err->output_message)(cinfo);
        if(cinfo->client_data) {
            auto & dmgr = *static_cast<CustomJpegDestMgr*>(cinfo->client_data);
            dmgr.success = false;
        }
    };

    jpeg_create_compress(cinfo.get());
    if (checkError("Error initializing compression")) {
        return 0;
    }

    dmgr.buffer = static_cast<JOCTET*>(frame.outputBuffer);
    dmgr.bufferSize = frame.outputBufferSize;
    dmgr.encodedSize = 0;
    dmgr.success = true;
    cinfo->client_data = static_cast<void*>(&dmgr);
    dmgr.init_destination = [](j_compress_ptr cinfo) {
        auto & dmgr = static_cast<CustomJpegDestMgr&>(*cinfo->dest);
        dmgr.next_output_byte = dmgr.buffer;
        dmgr.free_in_buffer = dmgr.bufferSize;
        ALOGV("%s:%d jpeg start: %p [%zu]", __FUNCTION__, __LINE__, dmgr.buffer, dmgr.bufferSize);
    };

    dmgr.empty_output_buffer = [](j_compress_ptr cinfo __unused) {
        ALOGV("%s:%d Out of buffer", __FUNCTION__, __LINE__);
        return 0;
    };

    dmgr.term_destination = [](j_compress_ptr cinfo) {
        auto & dmgr = static_cast<CustomJpegDestMgr&>(*cinfo->dest);
        dmgr.encodedSize = dmgr.bufferSize - dmgr.free_in_buffer;
        ALOGV("%s:%d Done with jpeg: %zu", __FUNCTION__, __LINE__, dmgr.encodedSize);
    };

    cinfo->dest = reinterpret_cast<struct jpeg_destination_mgr*>(&dmgr);

    // Set up compression parameters
    cinfo->image_width = frame.width;
    cinfo->image_height = frame.height;
    cinfo->input_components = 3;
    cinfo->in_color_space = JCS_YCbCr;

    jpeg_set_defaults(cinfo.get());
    if (checkError("Error configuring defaults")) {
        return 0;
    }

    jpeg_set_colorspace(cinfo.get(), JCS_YCbCr);
    if (checkError("Error configuring color space")) {
        return 0;
    }

    cinfo->raw_data_in = 1;
    // YUV420 planar with chroma subsampling
    cinfo->comp_info[0].h_samp_factor = 2;
    cinfo->comp_info[0].v_samp_factor = 2;
    cinfo->comp_info[1].h_samp_factor = 1;
    cinfo->comp_info[1].v_samp_factor = 1;
    cinfo->comp_info[2].h_samp_factor = 1;
    cinfo->comp_info[2].v_samp_factor = 1;

    int maxVSampFactor = std::max( {
        cinfo->comp_info[0].v_samp_factor,
        cinfo->comp_info[1].v_samp_factor,
        cinfo->comp_info[2].v_samp_factor
    });
    int cVSubSampling = cinfo->comp_info[0].v_samp_factor /
                        cinfo->comp_info[1].v_samp_factor;

    // Start compression
    jpeg_start_compress(cinfo.get(), TRUE);
    if (checkError("Error starting compression")) {
        return 0;
    }

    if ((frame.app1Buffer != nullptr) && (frame.app1BufferSize > 0)) {
        jpeg_write_marker(cinfo.get(), JPEG_APP0 + 1, static_cast<const JOCTET*>(frame.app1Buffer),
                frame.app1BufferSize);
    }

    // Compute our macroblock height, so we can pad our input to be vertically
    // macroblock aligned.

    size_t mcuV = DCTSIZE*maxVSampFactor;
    size_t paddedHeight = mcuV * ((cinfo->image_height + mcuV - 1) / mcuV);

    std::vector<JSAMPROW> yLines (paddedHeight);
    std::vector<JSAMPROW> cbLines(paddedHeight/cVSubSampling);
    std::vector<JSAMPROW> crLines(paddedHeight/cVSubSampling);

    uint8_t *py = static_cast<uint8_t*>(frame.yuvPlanes.imgY);
    uint8_t *pcr = static_cast<uint8_t*>(frame.yuvPlanes.imgCr);
    uint8_t *pcb = static_cast<uint8_t*>(frame.yuvPlanes.imgCb);

    for(uint32_t i = 0; i < paddedHeight; i++)
    {
        /* Once we are in the padding territory we still point to the last line
         * effectively replicating it several times ~ CLAMP_TO_EDGE */
        int li = std::min(i, cinfo->image_height - 1);
        yLines[i]  = static_cast<JSAMPROW>(py + li * frame.yuvPlanes.yStride);
        if(i < paddedHeight / cVSubSampling)
        {
            crLines[i] = static_cast<JSAMPROW>(pcr + li * frame.yuvPlanes.CbCrStride);
            cbLines[i] = static_cast<JSAMPROW>(pcb + li * frame.yuvPlanes.CbCrStride);
        }
    }

    const uint32_t batchSize = DCTSIZE * maxVSampFactor;
    while (cinfo->next_scanline < cinfo->image_height) {
        JSAMPARRAY planes[3]{ &yLines[cinfo->next_scanline],
                              &cbLines[cinfo->next_scanline/cVSubSampling],
                              &crLines[cinfo->next_scanline/cVSubSampling] };

        jpeg_write_raw_data(cinfo.get(), planes, batchSize);
        if (checkError("Error while compressing")) {
            return 0;
        }

        if (mJpegDone) {
            ALOGV("%s: Cancel called, exiting early", __FUNCTION__);
            jpeg_finish_compress(cinfo.get());
            return 0;
        }
    }

    jpeg_finish_compress(cinfo.get());
    if (checkError("Error while finishing compression")) {
        return 0;
    }

    return dmgr.encodedSize;
}

bool JpegCompressor::checkError(const char *msg) {
    if (mJpegErrorInfo) {
        char errBuffer[JMSG_LENGTH_MAX];
        mJpegErrorInfo->err->format_message(mJpegErrorInfo, errBuffer);
        ALOGE("%s: %s: %s", __FUNCTION__, msg, errBuffer);
        mJpegErrorInfo = NULL;
        return true;
    }

    return false;
}

}  // namespace android
