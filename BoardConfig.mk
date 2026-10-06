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

BOARD_KERNEL_BASE := 0x80000000
BOARD_KERNEL_PAGESIZE := 2048
BOARD_KERNEL_IMAGE_NAME := zImage
BOARD_KERNEL_CMDLINE := androidboot.hardware=$(TARGET_BOOTLOADER_BOARD_NAME)
BOARD_KERNEL_CMDLINE += androidboot.selinux=permissive
# CONFIG_CMDLINE_EXTEND puts this cmdline after CONFIG_CMDLINE's vmalloc=768M,
# so the later value sizes the vmalloc area. 384M leaves a 501 MB lowmem
# (Normal) zone instead of 108 MB, which keeps the in-kernel LMK from killing
# foreground apps; peak VmallocUsed stays near 90 MB with the SGX DDK 1.14.
BOARD_KERNEL_CMDLINE += vmalloc=384M
# omap_wdt pets WDT2 from its own delay IRQ by default, which keeps the board
# alive through any hang that leaves interrupts running. With kernelpet=0 the
# timer arms only when watchdogd opens /dev/watchdog, so a stalled userspace
# resets the board within watchdogd's interval plus margin.
BOARD_KERNEL_CMDLINE += omap_wdt.kernelpet=0
TARGET_KERNEL_CONFIG := tuna_defconfig
TARGET_KERNEL_SOURCE := kernel/samsung/tuna
# Kernel and SGX-KM build with Android Clang 22 (clang-r584948) under
# LTO_CLANG_THIN. kernel.mk puts the clang directory first in PATH and passes
# CC="ccache clang"; LLVM= names that same directory for ld.lld and the
# llvm binutils, which the ThinLTO link of bitcode objects requires, and
# LLVM_IAS=1 assembles with clang. CROSS_COMPILE keeps the
# arm-linux-androidkernel- prefix, from which the kernel Makefile derives
# the arm-linux-androideabi target. pvr-km.mk takes the same directory and
# IAS mode through PVR_KM_LLVM, so pvrsrvkm.ko and omaplfb.ko match the
# kernel's symbol CRCs.
# The device BoardConfig is read before vendor/lineage/config/BoardConfigKernel.mk
# defines BUILD_TOP, so abspath resolves the directory against the build top.
TARGET_KERNEL_CLANG_COMPILE := true
TARGET_KERNEL_CLANG_VERSION := r584948
KERNEL_LTO := thin
TUNA_KERNEL_LLVM := $(abspath prebuilts/clang/host/$(HOST_PREBUILT_TAG)/clang-$(TARGET_KERNEL_CLANG_VERSION)/bin)/
TARGET_KERNEL_ADDITIONAL_FLAGS := LLVM=$(TUNA_KERNEL_LLVM) LLVM_IAS=1 KCFLAGS=-Werror
PVR_KM_LLVM := $(TUNA_KERNEL_LLVM)
PVR_KM_LLVM_IAS := 1

# Treble: no vendor partition and no VNDK. /vendor is /system/vendor.
DEVICE_MANIFEST_FILE := $(DEVICE_FOLDER)/manifest.xml
PRODUCT_ENFORCE_VINTF_MANIFEST_OVERRIDE := true

# Old blobs
TARGET_NEEDS_PLATFORM_TEXT_RELOCATIONS := true
TARGET_PROCESS_SDK_VERSION_OVERRIDE := \
    /system/vendor/bin/hw/rild=22 \
    /system/vendor/lib/libsec-ril.so=22 \
    /system/vendor/lib/lib_gsd4t_jellybean.so=22
TARGET_LD_SHIM_LIBS := \
    /system/vendor/lib/lib_gsd4t_jellybean.so|libprocessgroup.so \
    /system/vendor/lib/lib_gsd4t_jellybean.so|libshim_gps_ssl.so

# Graphics
NUM_FRAMEBUFFER_SURFACE_BUFFERS := 3
TARGET_RUNNING_WITHOUT_SYNC_FRAMEWORK := true
TARGET_DISABLE_POSTRENDER_CLEANUP := true

# Camera
BOARD_CANT_REALLOCATE_OMX_BUFFERS := true
TARGET_CAMERASERVICE_CLOSES_NATIVE_HANDLES := true
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
# The REPIT table (system=2G, cache=128M) gives cache 131072 KiB (p11) and
# userdata sectors 4612096-30775295 (p12). A non-A/B 12 device keeps only
# OTA staging and recovery logs on /cache.
BOARD_CACHEIMAGE_PARTITION_SIZE := 134217728
BOARD_USERDATAIMAGE_PARTITION_SIZE := 13395558400
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

# Low RAM. MALLOC_SVELTE selects jemalloc5 without tcache over Scudo
# (bionic/libc/Android.bp). Every app on /system preopts: verify-filter apps
# keep their verified vdex on /system, so first boot skips verification and
# /data holds no dexopt copy, and SystemUI, the launcher and system_server
# map AOT code as clean, evictable file pages. The 2 GiB system partition
# holds the odex and vdex files. Prebuilt APKs (WebView, microG) compile on
# the device.
MALLOC_SVELTE := true
WITH_DEXPREOPT := true
DONT_DEXPREOPT_PREBUILTS := true

# SELinux. sepolicy/textrel holds the execmod grant for the two DT_TEXTREL
# blobs; it compiles only against a system/sepolicy that defines the
# textrel_vendor_lib_file attribute, and this one line removes it.
BOARD_VENDOR_SEPOLICY_DIRS += $(DEVICE_FOLDER)/sepolicy/vendor
BOARD_VENDOR_SEPOLICY_DIRS += $(DEVICE_FOLDER)/sepolicy/textrel

# Recovery. The non-A/B OTA generator reads partition devices from the
# recovery ramdisk fstab, so the build carries a recovery image; the
# installed recovery stays TWRP unless persist.vendor.recovery_update is set.
TARGET_RECOVERY_FSTAB := $(DEVICE_FOLDER)/rootdir/fstab.tuna
TARGET_RECOVERY_PIXEL_FORMAT := "BGRA_8888"
