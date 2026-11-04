LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := encored

LOCAL_C_INCLUDES := $(LOCAL_PATH)/include

LOCAL_STATIC_LIBRARIES := rapidjson spdlog InotifyWatcher GameRegistry EncoreUtility DeviceInfo BinderNDK BinderMonitor

LOCAL_SRC_FILES := $(wildcard $(LOCAL_PATH)/*.cpp)
LOCAL_SRC_FILES := $(LOCAL_SRC_FILES:$(LOCAL_PATH)/%=%)

LOCAL_CPPFLAGS += -fexceptions -std=c++23 -O2 -flto

# FAS (libfas only supports 64-bit targets)
ifeq ($(TARGET_ARCH_ABI),arm64-v8a)
LOCAL_STATIC_LIBRARIES += fas
LOCAL_CPPFLAGS += -DENCORE_HAVE_FAS
endif
LOCAL_CPPFLAGS += -Wpedantic -Wall -Wextra -Werror -Wformat -Wuninitialized

LOCAL_LDFLAGS += -flto

include $(BUILD_EXECUTABLE)

include $(LOCAL_PATH)/external/Android.mk $(LOCAL_PATH)/base/Android.mk
