LOCAL_PATH:= $(call my-dir)

ifneq ($(filter vsoc_x86 vsoc_x86_64 vsoc_arm64,$(TARGET_BOARD_PLATFORM)),)

include $(CLEAR_VARS)

LOCAL_MODULE := libgooglecamerahwl_impl
LOCAL_MODULE_OWNER := google
LOCAL_VENDOR_MODULE := true

LOCAL_CFLAGS += \
    -Wall \
    -Wextra \
    -Werror

LOCAL_SRC_FILES := \
    EmulatedCameraProviderHWLImpl.cpp \
    EmulatedCameraDeviceHWLImpl.cpp \
    EmulatedCameraDeviceSessionHWLImpl.cpp \
    EmulatedLogicalRequestState.cpp \
    EmulatedRequestProcessor.cpp \
    EmulatedRequestState.cpp \
    EmulatedScene.cpp \
    EmulatedSensor.cpp \
    EmulatedTorchState.cpp \
    JpegCompressor.cpp \
    utils/ExifUtils.cpp \
    utils/HWLUtils.cpp \
    utils/StreamConfigurationMap.cpp \

LOCAL_C_INCLUDES := \
    hardware/libhardware/include/hardware \

LOCAL_SHARED_LIBRARIES := \
    android.hardware.graphics.mapper@2.0 \
    android.hardware.graphics.mapper@3.0 \
    android.hardware.graphics.mapper@4.0 \
    android.hardware.camera.provider@2.4 \
    libbase \
    libcamera_metadata \
    libcutils \
    libexif \
    libgooglecamerahalutils \
    libhardware \
    libhidlbase \
    liblog \
    libjpeg \
    libsync \
    libjsoncpp \
    libutils \
    libyuv \

LOCAL_STATIC_LIBRARIES := \
    android.hardware.camera.common@1.0-helper

LOCAL_HEADER_LIBRARIES := \
    libgooglecamerahal_headers

include $(BUILD_SHARED_LIBRARY)

endif
