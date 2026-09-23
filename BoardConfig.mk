#
# Copyright (C) 2011 The Android Open-Source Project
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
#

DEVICE_FOLDER := device/samsung/tuna

# inherit from omap4
include hardware/ti/omap4/BoardConfigCommon.mk

TARGET_NO_BOOTLOADER := true
TARGET_NO_RADIOIMAGE := true

TARGET_BOARD_INFO_FILE := $(DEVICE_FOLDER)/board-info.txt
TARGET_BOOTLOADER_BOARD_NAME := tuna

# Binder API version
TARGET_USES_64_BIT_BINDER := true

# The kernel carries memfd_create (syscall 385) from the 3.17 series.
TARGET_HAS_MEMFD_BACKPORT := true

# Kernel: the 3.0 tree builds with GCC 4.9 only. The default toolchain is
# the absolute arm-linux-androidkernel- wrapper set in
# prebuilts/gcc/.../arm-linux-androideabi-4.9, which links to that GCC.
BOARD_KERNEL_BASE := 0x80000000
BOARD_KERNEL_PAGESIZE := 2048
BOARD_KERNEL_IMAGE_NAME := zImage
BOARD_KERNEL_CMDLINE := androidboot.hardware=$(TARGET_BOOTLOADER_BOARD_NAME)
BOARD_KERNEL_CMDLINE += androidboot.selinux=permissive
TARGET_KERNEL_CONFIG := tuna_defconfig
TARGET_KERNEL_SOURCE := kernel/samsung/tuna
TARGET_KERNEL_CLANG_COMPILE := false

# Treble: no vendor partition and no VNDK. /vendor is /system/vendor.
DEVICE_MANIFEST_FILE := $(DEVICE_FOLDER)/manifest.xml
PRODUCT_ENFORCE_VINTF_MANIFEST_OVERRIDE := true

# Old blobs
TARGET_NEEDS_PLATFORM_TEXT_RELOCATIONS := true
TARGET_PROCESS_SDK_VERSION_OVERRIDE := \
    /system/vendor/bin/hw/rild=22 \
    /system/vendor/lib/libsec-ril.so=22 \
    /system/vendor/lib/hw/gps.omap4.so=22
TARGET_LD_SHIM_LIBS := \
    /system/vendor/lib/hw/gps.omap4.so|libprocessgroup.so \
    /system/vendor/lib/hw/gps.omap4.so|libshim_gps_ssl.so

# Graphics
NUM_FRAMEBUFFER_SURFACE_BUFFERS := 3
TARGET_RUNNING_WITHOUT_SYNC_FRAMEWORK := true
TARGET_DISABLE_POSTRENDER_CLEANUP := true

# Camera
BOARD_CANT_REALLOCATE_OMX_BUFFERS := true
TARGET_CAMERASERVICE_CLOSES_NATIVE_HANDLES := true
TARGET_HAS_LEGACY_CAMERA_HAL1 := true
TI_CAMERAHAL_USES_LEGACY_DOMX_DCC := true
TI_CAMERAHAL_INTERFACE := OMX
DOMX_TUNA := true
TI_CAMERAHAL_MAX_CAMERAS_SUPPORTED := 2

# Filesystem
TARGET_USERIMAGES_USE_EXT4 := true
TARGET_USERIMAGES_USE_F2FS := true
BOARD_BOOTIMAGE_PARTITION_SIZE := 8388608
BOARD_RECOVERYIMAGE_PARTITION_SIZE := 12517376
BOARD_CACHEIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_CACHEIMAGE_PARTITION_SIZE := 452984832
BOARD_USERDATAIMAGE_PARTITION_SIZE := 14539537408
BOARD_FLASH_BLOCK_SIZE := 4096

# System is 2 GiB after the REPIT repartition (system=2G). The stock
# table holds 685768704 bytes (654 MiB); TUNA_STOCK_SYSTEM_PARTITION=true
# selects it for an image that fits there.
ifeq ($(TUNA_STOCK_SYSTEM_PARTITION),true)
BOARD_SYSTEMIMAGE_PARTITION_SIZE := 685768704
else
BOARD_SYSTEMIMAGE_PARTITION_SIZE := 2147483648
endif
BOARD_SYSTEMIMAGE_JOURNAL_SIZE := 0

BOARD_ROOT_EXTRA_FOLDERS := factory tee

# Wifi
BOARD_WLAN_DEVICE                := bcmdhd
WPA_SUPPLICANT_VERSION           := VER_0_8_X
BOARD_WPA_SUPPLICANT_DRIVER      := NL80211
BOARD_WPA_SUPPLICANT_PRIVATE_LIB := lib_driver_cmd_$(BOARD_WLAN_DEVICE)
BOARD_HOSTAPD_DRIVER             := NL80211
BOARD_HOSTAPD_PRIVATE_LIB        := lib_driver_cmd_$(BOARD_WLAN_DEVICE)
WIFI_DRIVER_FW_PATH_PARAM        := "/sys/module/bcmdhd/parameters/firmware_path"
WIFI_DRIVER_FW_PATH_STA          := "/vendor/firmware/fw_bcmdhd.bin"
WIFI_DRIVER_FW_PATH_AP           := "/vendor/firmware/fw_bcmdhd_apsta.bin"
WIFI_HIDL_UNIFIED_SUPPLICANT_SERVICE_RC_ENTRY := true

# Bluetooth
BOARD_HAVE_BLUETOOTH := true
BOARD_HAVE_BLUETOOTH_BCM := true
BOARD_BLUETOOTH_BDROID_BUILDCFG_INCLUDE_DIR := $(DEVICE_FOLDER)/bluetooth
BOARD_CUSTOM_BT_CONFIG := $(DEVICE_FOLDER)/bluetooth/vnd_tuna.txt

# Network routing
TARGET_NEEDS_NETD_DIRECT_CONNECT_RULE := true

TARGET_TUNA_AUDIO_HDMI := true

# Sensors: the compass on toro(plus) needs the PMIC noise filter; maguro
# tolerates it.
BOARD_INVENSENSE_APPLY_COMPASS_NOISE_FILTER := true

# Low RAM
MALLOC_SVELTE := true
WITH_DEXPREOPT_BOOT_IMG_AND_SYSTEM_SERVER_ONLY := true

# SELinux
BOARD_VENDOR_SEPOLICY_DIRS += $(DEVICE_FOLDER)/sepolicy/vendor

# Recovery. The non-A/B OTA generator reads partition devices from the
# recovery ramdisk fstab, so the build carries a recovery image; the
# installed recovery stays TWRP unless persist.vendor.recovery_update is set.
TARGET_RECOVERY_FSTAB := $(DEVICE_FOLDER)/rootdir/fstab.tuna
TARGET_RECOVERY_PIXEL_FORMAT := "BGRA_8888"
