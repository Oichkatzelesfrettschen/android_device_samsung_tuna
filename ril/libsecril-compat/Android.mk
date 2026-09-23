LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE_TAGS := optional

LOCAL_SRC_FILES := \
	md5.c \
	secril-compat.c

LOCAL_SHARED_LIBRARIES := \
	libhardware_legacy \
	libbinder \
	liblog

LOCAL_CFLAGS := -Wall -Werror

LOCAL_MODULE := libsecril-compat

LOCAL_VENDOR_MODULE := true
include $(BUILD_SHARED_LIBRARY)
