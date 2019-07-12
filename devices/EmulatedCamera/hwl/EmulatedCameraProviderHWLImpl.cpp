/*
 * Copyright (C) 2013-2019 The Android Open Source Project
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
#include <android-base/file.h>
#include <android-base/strings.h>
#include <log/log.h>

#include "camera_common.h"
#include "EmulatedCameraProviderHWLImpl.h"
#include "EmulatedCameraDeviceHWLImpl.h"
#include "EmulatedCameraDeviceSessionHWLImpl.h"
#include "EmulatedSensor.h"
#include "utils/HWLUtils.h"
#include "vendor_tag_defs.h"

namespace android {

// Location of the camera configuration files.
const char* EmulatedCameraProviderHwlImpl::kConfigurationFileLocation =
        "/vendor/etc/config/camera.json";

// Array of camera definitions for all cameras available on the device (array).
// Top Level Key.
const char* EmulatedCameraProviderHwlImpl::kCameraDefinitionsKey = "camera_definitions";

std::unique_ptr<EmulatedCameraProviderHwlImpl> EmulatedCameraProviderHwlImpl::Create() {
    auto provider =
            std::unique_ptr<EmulatedCameraProviderHwlImpl>(new EmulatedCameraProviderHwlImpl());

    if (provider == nullptr) {
        ALOGE("%s: Creating EmulatedCameraProviderHwlImpl failed.", __FUNCTION__);
        return nullptr;
    }

    status_t res = provider->initialize();
    if (res != OK) {
        ALOGE("%s: Initializing EmulatedCameraProviderHwlImpl failed: %s (%d).",
                __FUNCTION__, strerror(-res), res);
        return nullptr;
    }

    ALOGI("%s: Created EmulatedCameraProviderHwlImpl", __FUNCTION__);

    return provider;
}

status_t EmulatedCameraProviderHwlImpl::getTagFromName(const char *name, uint32_t *tag) {
    if (name == nullptr || tag == nullptr) {
        return BAD_VALUE;
    }

    size_t nameLength = strlen(name);
    // First, find the section by the longest string match
    const char *section = NULL;
    size_t sectionIndex = 0;
    size_t sectionLength = 0;
    for (size_t i = 0; i < ANDROID_SECTION_COUNT; ++i) {
        const char *str = camera_metadata_section_names[i];

        ALOGV("%s: Trying to match against section '%s'", __FUNCTION__, str);

        if (strstr(name, str) == name) { // name begins with the section name
            size_t strLength = strlen(str);

            ALOGV("%s: Name begins with section name", __FUNCTION__);

            // section name is the longest we've found so far
            if (section == NULL || sectionLength < strLength) {
                section = str;
                sectionIndex = i;
                sectionLength = strLength;

                ALOGV("%s: Found new best section (%s)", __FUNCTION__, section);
            }
        }
    }

    if (section == NULL) {
        return NAME_NOT_FOUND;
    } else {
        ALOGV("%s: Found matched section '%s' (%zu)",
              __FUNCTION__, section, sectionIndex);
    }

    // Get the tag name component of the name
    const char *nameTagName = name + sectionLength + 1; // x.y.z -> z
    if (sectionLength + 1 >= nameLength) {
        return BAD_VALUE;
    }

    // Match rest of name against the tag names in that section only
    uint32_t candidateTag = 0;
    // Match built-in tags (typically android.*)
    uint32_t tagBegin, tagEnd; // [tagBegin, tagEnd)
    tagBegin = camera_metadata_section_bounds[sectionIndex][0];
    tagEnd = camera_metadata_section_bounds[sectionIndex][1];

    for (candidateTag = tagBegin; candidateTag < tagEnd; ++candidateTag) {
        const char *tagName = get_camera_metadata_tag_name(candidateTag);

        if (strcmp(nameTagName, tagName) == 0) {
            ALOGV("%s: Found matched tag '%s' (%d)",
                    __FUNCTION__, tagName, candidateTag);
            break;
        }
    }

    if (candidateTag == tagEnd) {
        return NAME_NOT_FOUND;
    }

    *tag = candidateTag;
    return OK;
}

status_t getUInt8Value(const Json::Value& value, uint8_t *result/*out*/) {
    if (result == nullptr) {
        return BAD_VALUE;
    }

    if (value.isString()) {
        errno = 0;
        auto intValue = strtol(value.asCString(), nullptr, 10);
        if ((intValue >= 0) && (intValue <= UINT8_MAX) && (errno == 0)) {
            *result = intValue;
        }
    } else if (value.isUInt()) {
        *result = value.asUInt();
    } else {
        ALOGE("%s: json type: %d doesn't match with byte tag type",
                __FUNCTION__, value.type());
        return BAD_VALUE;
    }

    return OK;
}

status_t getInt32Value(const Json::Value& value, int32_t *result/*out*/) {
    if (result == nullptr) {
        return BAD_VALUE;
    }

    if (value.isString()) {
        errno = 0;
        auto intValue = strtol(value.asCString(), nullptr, 10);
        if ((intValue >= INT32_MIN) && (intValue <= INT32_MAX) && (errno == 0)) {
            *result = intValue;
        }
    } else if (value.isInt()) {
        *result = value.asInt();
    } else {
        ALOGE("%s: json type: %d doesn't match with int32 tag type",
                __FUNCTION__, value.type());
        return BAD_VALUE;
    }

    return OK;
}

status_t getInt64Value(const Json::Value& value, int64_t *result/*out*/) {
    if (result == nullptr) {
        return BAD_VALUE;
    }

    if (value.isString()) {
        errno = 0;
        auto intValue = strtol(value.asCString(), nullptr, 10);
        if ((intValue >= INT64_MIN) && (intValue <= INT64_MAX) && (errno == 0)) {
            *result = intValue;
        }
    } else if (value.isInt64()) {
        *result = value.asInt64();
    } else {
        ALOGE("%s: json type: %d doesn't match with int64 tag type",
                __FUNCTION__, value.type());
        return BAD_VALUE;
    }

    return OK;
}

status_t getFloatValue(const Json::Value& value, float *result/*out*/) {
    if (result == nullptr) {
        return BAD_VALUE;
    }

    if (value.isString()) {
        errno = 0;
        auto floatValue = strtof(value.asCString(), nullptr);
        if (errno == 0) {
            *result = floatValue;
        }
    } else if (value.isDouble()) { //JsonCPP doesn't seem to support floats.
        *result = value.asDouble();
    } else {
        ALOGE("%s: json type: %d doesn't match with float tag type",
                __FUNCTION__, value.type());
        return BAD_VALUE;
    }

    return OK;
}

status_t getDoubleValue(const Json::Value& value, double *result/*out*/) {
    if (result == nullptr) {
        return BAD_VALUE;
    }

    if (value.isString()) {
        errno = 0;
        auto floatValue = strtod(value.asCString(), nullptr);
        if (errno == 0) {
            *result = floatValue;
        }
    } else if (value.isDouble()) {
        *result = value.asDouble();
    } else {
        ALOGE("%s: json type: %d doesn't match with double tag type",
                __FUNCTION__, value.type());
        return BAD_VALUE;
    }

    return OK;
}

template<typename T, typename funcType>
status_t insertTag(const Json::Value& jsonValue, uint32_t tagId, funcType getValFunc,
        HalCameraMetadata *meta/*out*/) {
    if (meta == nullptr) {
        return BAD_VALUE;
    }

    std::vector<T> values;
    T result;
    status_t ret = OK;
    if (jsonValue.isArray()) {
        values.reserve(jsonValue.size());
        for (const auto& val : jsonValue) {
            ret = getValFunc(val, &result);
            if (ret == OK) {
                values.push_back(result);
            }
        }
    } else {
        ret = getValFunc(jsonValue, &result);
        if (ret == OK) {
            values.push_back(result);
        }
    }

    if (ret == OK) {
        ret = meta->Set(tagId, values.data(), values.size());
    }

    return ret;
}

status_t insertRationalTag(const Json::Value& jsonValue, uint32_t tagId,
        HalCameraMetadata *meta/*out*/) {
    if (meta == nullptr) {
        return BAD_VALUE;
    }

    std::vector<camera_metadata_rational_t> values;
    status_t ret = OK;
    if (jsonValue.isArray() && ((jsonValue.size() % 2) == 0)) {
        values.reserve(jsonValue.size() / 2);
        auto it = jsonValue.begin();
        while (it != jsonValue.end()) {
            camera_metadata_rational_t result;
            ret = getInt32Value((*it), &result.numerator); it++;
            ret |= getInt32Value((*it), &result.denominator); it++;
            if (ret != OK) {
                break;
            }
            values.push_back(result);
        }
    } else {
        ALOGE("%s: json type: %d doesn't match with rational tag type", __FUNCTION__,
                jsonValue.type());
        return BAD_VALUE;
    }

    if (ret == OK) {
        ret = meta->Set(tagId, values.data(), values.size());
    }

    return ret;
}

status_t EmulatedCameraProviderHwlImpl::parseCharacteristics(const Json::Value& value) {
    if (!value.isObject()) {
        ALOGE("%s: Configuration root is not an object", __FUNCTION__);
        return false;
    }

    auto staticMeta = HalCameraMetadata::Create(1, 10);
    auto members = value.getMemberNames();
    for (const auto& member: members) {
        uint32_t tagId;
        auto stat = getTagFromName(member.c_str(), &tagId);
        if (stat != OK) {
            ALOGE("%s: tag %s not supported, skipping!", __func__, member.c_str());
            continue;
        }

        auto tagType = get_camera_metadata_tag_type(tagId);
        auto tagValue = value[member.c_str()];
        switch (tagType) {
            case TYPE_BYTE:
                insertTag<uint8_t>(tagValue, tagId, getUInt8Value, staticMeta.get());
                break;
            case TYPE_INT32:
                insertTag<int32_t>(tagValue, tagId, getInt32Value, staticMeta.get());
                break;
            case TYPE_INT64:
                insertTag<int64_t>(tagValue, tagId, getInt64Value, staticMeta.get());
                break;
            case TYPE_FLOAT:
                insertTag<float>(tagValue, tagId, getFloatValue, staticMeta.get());
                break;
            case TYPE_DOUBLE:
                insertTag<double>(tagValue, tagId, getDoubleValue, staticMeta.get());
                break;
            case TYPE_RATIONAL:
                insertRationalTag(tagValue, tagId, staticMeta.get());
                break;
            default:
                ALOGE("%s: Unsupported tag type: %d!", __FUNCTION__, tagType);
        }
    }

    SensorCharacteristics sensorCharacteristics;
    auto ret = getSensorCharacteristics(staticMeta.get(), &sensorCharacteristics);
    if (ret != OK) {
        ALOGE("%s: Unable to extract sensor characteristics!", __FUNCTION__);
        return ret;
    }

    if (!EmulatedSensor::areCharacteristicsSupported(sensorCharacteristics)) {
        ALOGE("%s: Sensor characteristics not supported!", __FUNCTION__);
        return BAD_VALUE;
    }

    // TODO: This probably should not be expected by GCH from every HWL impl.
    //       Adding anyhow to pass CTS
    int32_t payloadFrames = 0;
    staticMeta->Set(google_camera_hal::kHdrplusPayloadFrames, &payloadFrames, 1);

    mStaticMetadata.push_back(std::move(staticMeta));

    return OK;
}

status_t EmulatedCameraProviderHwlImpl::initialize() {
    std::string config;
    if (!android::base::ReadFileToString(kConfigurationFileLocation, &config)) {
        ALOGE("%s: Could not open configuration file: %s", __FUNCTION__,
                kConfigurationFileLocation);
        return false;
    }

    Json::Reader configReader;
    Json::Value root;
    if (!configReader.parse(config, root)) {
        ALOGE("Could not parse configuration file: %s",
                configReader.getFormattedErrorMessages().c_str());
        return BAD_VALUE;
    }

    return parseCharacteristics(root);
}

status_t EmulatedCameraProviderHwlImpl::SetCallback(
    const HwlCameraProviderCallback& /*callback*/) {
    // TODO: set callbacks
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

    for (size_t cameraId = 0; cameraId < mStaticMetadata.size(); cameraId++) {
        camera_ids->push_back(cameraId);
    }

    return OK;
}

status_t EmulatedCameraProviderHwlImpl::CreateCameraDeviceHwl(
    uint32_t cameraId, std::unique_ptr<CameraDeviceHwl>* camera_device_hwl) {
    if (camera_device_hwl == nullptr) {
        ALOGE("%s: camera_device_hwl is nullptr.", __FUNCTION__);
        return BAD_VALUE;
    }

    if (cameraId >= mStaticMetadata.size()) {
        return BAD_VALUE;
    }

    std::unique_ptr<HalCameraMetadata> meta = HalCameraMetadata::Clone(
            mStaticMetadata[cameraId].get());
    *camera_device_hwl = EmulatedCameraDeviceHwlImpl::Create(cameraId, std::move(meta));
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

    //TODO: Initialize an emulated buffer allocator

    return OK;
}

}  // namespace android
