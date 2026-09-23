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

# Definitions shared by every tuna product.

DEVICE_FOLDER := device/samsung/tuna

# OMAP4460 selects the SGX540_120 graphics blobs in vendor/ti.
TARGET_BOARD_OMAP_CPU := 4460

$(call inherit-product, hardware/ti/omap4/omap4.mk)

# The shipping level selects the VINTF framework compatibility matrix.
$(call inherit-product, $(SRC_TARGET_DIR)/product/product_launched_with_k.mk)

DEVICE_PACKAGE_OVERLAYS += $(DEVICE_FOLDER)/overlay

PRODUCT_AAPT_CONFIG := normal
PRODUCT_AAPT_PREF_CONFIG := xhdpi
PRODUCT_CHARACTERISTICS := nosdcard

# Init and first-stage mount. The first-stage init in the boot ramdisk
# reads fstab.<androidboot.hardware> from the ramdisk root.
PRODUCT_COPY_FILES += \
    $(DEVICE_FOLDER)/rootdir/fstab.tuna:$(TARGET_COPY_OUT_RAMDISK)/fstab.tuna \
    $(DEVICE_FOLDER)/rootdir/fstab.tuna:$(TARGET_COPY_OUT_VENDOR)/etc/fstab.tuna \
    $(DEVICE_FOLDER)/rootdir/init.tuna.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/hw/init.tuna.rc \
    $(DEVICE_FOLDER)/rootdir/init.tuna.usb.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/hw/init.tuna.usb.rc \
    $(DEVICE_FOLDER)/rootdir/ueventd.tuna.rc:$(TARGET_COPY_OUT_VENDOR)/ueventd.rc \
    $(DEVICE_FOLDER)/rootdir/tee-fs-setup.sh:$(TARGET_COPY_OUT_VENDOR)/bin/tee-fs-setup.sh

# Audio
PRODUCT_PACKAGES += \
    android.hardware.audio@6.0-impl \
    android.hardware.audio.effect@6.0-impl \
    android.hardware.audio.service \
    audio.primary.tuna \
    audio.a2dp.default \
    audio.usb.default \
    audio.r_submix.default

PRODUCT_COPY_FILES += \
    frameworks/av/media/libeffects/data/audio_effects.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_effects.xml \
    $(DEVICE_FOLDER)/audio/policy/audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_configuration.xml \
    frameworks/av/services/audiopolicy/config/a2dp_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/a2dp_audio_policy_configuration.xml \
    frameworks/av/services/audiopolicy/config/usb_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/usb_audio_policy_configuration.xml \
    frameworks/av/services/audiopolicy/config/r_submix_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/r_submix_audio_policy_configuration.xml \
    frameworks/av/services/audiopolicy/config/audio_policy_volumes.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_volumes.xml \
    frameworks/av/services/audiopolicy/config/default_volume_tables.xml:$(TARGET_COPY_OUT_VENDOR)/etc/default_volume_tables.xml

# Bluetooth
PRODUCT_PACKAGES += \
    android.hardware.bluetooth@1.0-impl \
    android.hardware.bluetooth@1.0-service \
    libbt-vendor

# Camera: HAL1 behind the legacy provider
PRODUCT_PACKAGES += \
    android.hardware.camera.provider@2.4-impl-legacy \
    android.hardware.camera.provider@2.4-service \
    camera.device@1.0-impl-legacy \
    camera.omap4 \
    Snap

PRODUCT_PROPERTY_OVERRIDES += \
    camera.disable_zsl_mode=1 \
    media.stagefright.legacyencoder=true \
    media.stagefright.less-secure=true

# DRM
PRODUCT_PACKAGES += \
    android.hardware.drm@1.0-impl \
    android.hardware.drm@1.0-service

# GNSS: the SiRF blob behind the tuna shim
PRODUCT_PACKAGES += \
    android.hardware.gnss@1.0-impl \
    android.hardware.gnss@1.0-service \
    gps.tuna \
    libshim_gps_ssl

PRODUCT_COPY_FILES += \
    $(DEVICE_FOLDER)/gps/gps.conf:$(TARGET_COPY_OUT_VENDOR)/etc/gps.conf

# Graphics: gralloc and hwcomposer are the DDK 1.14 blobs, loaded in
# process through the passthrough allocator, mapper and composer.
PRODUCT_PACKAGES += \
    android.hardware.graphics.allocator@2.0-impl \
    android.hardware.graphics.composer@2.1-impl \
    android.hardware.graphics.mapper@2.0-impl \
    android.hardware.memtrack@1.0-impl \
    android.hardware.memtrack@1.0-service

# DDK 1.14 advertises OpenGL ES 2.0 only.
PRODUCT_PROPERTY_OVERRIDES += \
    ro.opengles.version=131072 \
    ro.hardware.egl=POWERVR_SGX540_120 \
    debug.renderengine.backend=gles \
    debug.hwui.renderer=opengl \
    ro.zygote.disable_gl_preload=true \
    ro.bq.gpu_to_cpu_unsupported=1 \
    ro.sf.lcd_density=320 \
    debug.vfr.enable=0 \
    persist.hwc.bltpolicy=0

# Health
PRODUCT_PACKAGES += \
    android.hardware.health@2.1-impl \
    android.hardware.health@2.1-service

# Keymaster: the TrustZone keystore through the keymaster0 wrapper
PRODUCT_PACKAGES += \
    android.hardware.keymaster@3.0-impl \
    android.hardware.keymaster@3.0-service \
    keystore.tuna

PRODUCT_PACKAGES += \
    android.hardware.gatekeeper@1.0-service.software

# Lights
PRODUCT_PACKAGES += \
    android.hardware.light@2.0-impl \
    android.hardware.light@2.0-service \
    lights.tuna

# Media
PRODUCT_COPY_FILES += \
    $(DEVICE_FOLDER)/media_profiles.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_profiles_V1_0.xml \
    $(DEVICE_FOLDER)/media_codecs.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs.xml \
    frameworks/av/media/libstagefright/data/media_codecs_google_audio.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs_google_audio.xml \
    frameworks/av/media/libstagefright/data/media_codecs_google_telephony.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs_google_telephony.xml \
    frameworks/av/media/libstagefright/data/media_codecs_google_video.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs_google_video.xml

PRODUCT_PROPERTY_OVERRIDES += \
    debug.stagefright.ccodec=0 \
    persist.media.treble_omx=false

# Power
PRODUCT_PACKAGES += \
    android.hardware.power@1.0-impl \
    android.hardware.power@1.0-service

# RIL
PRODUCT_PACKAGES += \
    libsecril-client \
    libsecril-compat \
    libsecril-shim

# Sensors
PRODUCT_PACKAGES += \
    android.hardware.sensors@1.0-impl \
    android.hardware.sensors@1.0-service \
    sensors.tuna

# USB
PRODUCT_PACKAGES += \
    android.hardware.usb@1.0-service.basic

PRODUCT_PROPERTY_OVERRIDES += \
    ro.adb.nonblocking_ffs=false

# Vibrator: the timed_output vibrator through vibrator.default
PRODUCT_PACKAGES += \
    android.hardware.vibrator@1.0-impl \
    android.hardware.vibrator@1.0-service \
    vibrator.default

# Wifi
PRODUCT_PACKAGES += \
    android.hardware.wifi@1.0-service-lazy.legacy \
    hostapd \
    libwpa_client \
    wificond \
    wpa_supplicant \
    wpa_supplicant.conf

PRODUCT_COPY_FILES += \
    $(DEVICE_FOLDER)/wifi/wpa_supplicant_overlay.conf:$(TARGET_COPY_OUT_VENDOR)/etc/wifi/wpa_supplicant_overlay.conf \
    $(DEVICE_FOLDER)/wifi/p2p_supplicant_overlay.conf:$(TARGET_COPY_OUT_VENDOR)/etc/wifi/p2p_supplicant_overlay.conf

PRODUCT_PROPERTY_OVERRIDES += \
    wifi.interface=wlan0

# Symlinks
PRODUCT_PACKAGES += \
    tuna_hdcp_keys

# Filesystem tools
PRODUCT_PACKAGES += \
    e2fsck \
    fsck.f2fs \
    mkfs.f2fs

# Key maps
PRODUCT_COPY_FILES += \
    $(DEVICE_FOLDER)/keymap/tuna-gpio-keypad.kl:$(TARGET_COPY_OUT_VENDOR)/usr/keylayout/tuna-gpio-keypad.kl \
    $(DEVICE_FOLDER)/keymap/tuna-gpio-keypad.kcm:$(TARGET_COPY_OUT_VENDOR)/usr/keychars/tuna-gpio-keypad.kcm \
    $(DEVICE_FOLDER)/keymap/sec_jack.kl:$(TARGET_COPY_OUT_VENDOR)/usr/keylayout/sec_jack.kl \
    $(DEVICE_FOLDER)/keymap/sec_jack.kcm:$(TARGET_COPY_OUT_VENDOR)/usr/keychars/sec_jack.kcm \
    $(DEVICE_FOLDER)/keymap/sii9234_rcp.kl:$(TARGET_COPY_OUT_VENDOR)/usr/keylayout/sii9234_rcp.kl \
    $(DEVICE_FOLDER)/keymap/sii9234_rcp.kcm:$(TARGET_COPY_OUT_VENDOR)/usr/keychars/sii9234_rcp.kcm \
    $(DEVICE_FOLDER)/touchscreen/Melfas_MMSxxx_Touchscreen.idc:$(TARGET_COPY_OUT_VENDOR)/usr/idc/Melfas_MMSxxx_Touchscreen.idc

# Hardware features. The Go profile supplies handheld_core_hardware.xml.
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.bluetooth_le.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.bluetooth_le.xml \
    frameworks/native/data/etc/android.hardware.camera.flash-autofocus.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.camera.flash-autofocus.xml \
    frameworks/native/data/etc/android.hardware.camera.front.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.camera.front.xml \
    frameworks/native/data/etc/android.hardware.location.gps.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.location.gps.xml \
    frameworks/native/data/etc/android.hardware.wifi.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.wifi.xml \
    frameworks/native/data/etc/android.hardware.wifi.direct.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.wifi.direct.xml \
    frameworks/native/data/etc/android.hardware.sensor.accelerometer.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.accelerometer.xml \
    frameworks/native/data/etc/android.hardware.sensor.barometer.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.barometer.xml \
    frameworks/native/data/etc/android.hardware.sensor.compass.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.compass.xml \
    frameworks/native/data/etc/android.hardware.sensor.gyroscope.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.gyroscope.xml \
    frameworks/native/data/etc/android.hardware.sensor.light.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.light.xml \
    frameworks/native/data/etc/android.hardware.sensor.proximity.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.proximity.xml \
    frameworks/native/data/etc/android.hardware.touchscreen.multitouch.jazzhand.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.touchscreen.multitouch.jazzhand.xml \
    frameworks/native/data/etc/android.hardware.usb.accessory.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.usb.accessory.xml \
    frameworks/native/data/etc/android.hardware.usb.host.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.usb.host.xml \
    frameworks/native/data/etc/android.software.sip.voip.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.software.sip.voip.xml

# Low RAM. ro.config.low_ram (from the Go profile) turns off hardware UI
# rendering, which artifacts on SGX540; force_highendgfx restores it.
PRODUCT_PROPERTY_OVERRIDES += \
    persist.sys.force_highendgfx=true \
    config.disable_atlas=true \
    dalvik.vm.dex2oat-threads=1 \
    pm.dexopt.shared=quicken \
    ro.config.max_starting_bg=1 \
    ro.config.small_battery=true

$(call inherit-product, frameworks/native/build/phone-xhdpi-1024-dalvik-heap.mk)

$(call inherit-product, hardware/broadcom/wlan/bcmdhd/firmware/bcm4330/device-bcm.mk)

$(call inherit-product, vendor/samsung/tuna/tuna-vendor.mk)
