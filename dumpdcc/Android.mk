LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := dumpdcc
LOCAL_VENDOR_MODULE := true
LOCAL_SRC_FILES := dumpdcc.c
LOCAL_SHARED_LIBRARIES := liblog
LOCAL_MODULE_TAGS := optional
LOCAL_HEADER_LIBRARIES := libhardware_headers libcutils_headers liblog_headers libutils_headers
LOCAL_CFLAGS := -Wall -Werror

include $(BUILD_EXECUTABLE)
