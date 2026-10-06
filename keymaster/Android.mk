# Copyright (C) 2011 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

ifeq ($(TARGET_BOARD_PLATFORM),omap4)
ifeq ($(BOARD_USES_SECURE_SERVICES),true)

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := keystore.tuna

LOCAL_MODULE_RELATIVE_PATH := hw
LOCAL_VENDOR_MODULE := true

LOCAL_SRC_FILES := \
	keymaster_tuna.cpp

LOCAL_C_INCLUDES := \
	hardware/ti/omap4/security/tf_sdk/include

LOCAL_CFLAGS := -fvisibility=hidden -Wall -Wextra -Werror
ifeq ($(call math_gt_or_eq,$(PLATFORM_SDK_VERSION),31),true)
LOCAL_CFLAGS += -DTUNA_SOFT_KEYMASTER_HAS_VERSION=1
else
LOCAL_CFLAGS += -DTUNA_SOFT_KEYMASTER_HAS_VERSION=0
endif

LOCAL_HEADER_LIBRARIES := libhardware_headers

LOCAL_SHARED_LIBRARIES := libcutils liblog libcrypto libtf_crypto_sst \
    libsoftkeymasterdevice libkeymaster_portable libkeymaster_messages

LOCAL_MODULE_TAGS := optional

include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := keystore_tuna_km1_probe
LOCAL_VENDOR_MODULE := true
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := keystore_tuna_km1_probe.cpp
LOCAL_CFLAGS := -Wall -Wextra -Werror
LOCAL_HEADER_LIBRARIES := libhardware_headers
LOCAL_SHARED_LIBRARIES := libkeymaster3device android.hardware.keymaster@3.0 \
    libhidlbase libutils libcrypto liblog libdl
ifeq ($(call math_lt,$(PLATFORM_SDK_VERSION),30),true)
LOCAL_SHARED_LIBRARIES += libhidltransport libhwbinder
endif
include $(BUILD_EXECUTABLE)

endif # ifeq ($(BOARD_USES_SECURE_SERVICES),true)
endif # ifeq ($(TARGET_BOARD_PLATFORM),omap4)
