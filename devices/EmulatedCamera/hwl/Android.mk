LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MULTILIB := 64
#TODO: Library name should include emulator
LOCAL_MODULE := libgooglecamerahwl_impl
LOCAL_MODULE_OWNER := google
LOCAL_VENDOR_MODULE := true

LOCAL_SRC_FILES := \
    EmulatedCameraProviderHWLImpl.cpp \
    EmulatedCameraDeviceHWLImpl.cpp \
    EmulatedScene.cpp \
    EmulatedSensor.cpp \
    HWLUtils.cpp \
    JpegCompressor.cpp

LOCAL_C_INCLUDES := \
    external/libjpeg-turbo \
    hardware/libhardware/include/hardware \
    vendor/google/camera/common/hal/common \
    vendor/google/camera/common/hal/hwl_interface \
    vendor/google/camera/common/hal/utils

LOCAL_SHARED_LIBRARIES := \
    android.hardware.camera.common@1.0 \
    android.hardware.camera.provider@2.4 \
    libcamera_metadata \
    libcutils \
    libgooglecamerahalutils \
    libhardware \
    liblog \
    libjpeg \
    libjsoncpp \
    libbase \
    libutils

include $(BUILD_SHARED_LIBRARY)
