LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := rapidjson
LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)/rapidjson/include
include $(BUILD_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := spdlog
LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)/spdlog/include
include $(BUILD_STATIC_LIBRARY)

# libfas (Encore FAS userspace library, LKM backend only, 64-bit only)
ifeq ($(TARGET_ARCH_ABI),arm64-v8a)
include $(CLEAR_VARS)
LOCAL_MODULE := fas
FAS_ROOT := encore_fas
LOCAL_SRC_FILES := \
	$(FAS_ROOT)/lib/src/fas.c \
	$(FAS_ROOT)/lib/src/fas_elf.c \
	$(FAS_ROOT)/lib/src/fas_frame.c \
	$(FAS_ROOT)/lib/src/be_lkm.c \
	$(FAS_ROOT)/lib/src/be_replay.c \
	$(FAS_ROOT)/lib/src/be_bpf.c \
	$(FAS_ROOT)/lib/src/ringbuf.c
LOCAL_C_INCLUDES := \
	$(LOCAL_PATH)/$(FAS_ROOT)/lib/include \
	$(LOCAL_PATH)/$(FAS_ROOT)/include \
	$(LOCAL_PATH)/$(FAS_ROOT)/lib/src
LOCAL_EXPORT_C_INCLUDES := \
	$(LOCAL_PATH)/$(FAS_ROOT)/lib/include \
	$(LOCAL_PATH)/$(FAS_ROOT)/include
LOCAL_CFLAGS += -std=gnu11 -O2 -fvisibility=hidden -Wno-unused-parameter
include $(BUILD_STATIC_LIBRARY)
endif
