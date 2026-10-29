# SPDX-License-Identifier: Apache-2.0
# This private kiosk product omits the public-signed CTS shim and fallback
# stub APKs. Full CTS compatibility for those test cases is not claimed.
PRODUCT_PACKAGES += t380-cts-shim-exclusion

$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/full_base.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/product_launched_with_n_mr1.mk)
$(call inherit-product, vendor/lineage/config/common_full_tablet_wifionly.mk)
T380_ENABLE_QCOM_GRAPHICS := true
T380_ENABLE_WIFI := true
$(call inherit-product, device/samsung/gta2swifi/device.mk)
$(call inherit-product, vendor/samsung/gta2swifi/gta2swifi-graphics-vendor.mk)
# Learned wall-clock offset over the read-only PM8937 RTC. This is a DATA
# recovery hint, not trusted time. Standard network time remains enabled.
PRODUCT_PACKAGES += timekeep TimeKeep
WIFI_NEXT_PATH := device/samsung/gta2swifi/wifi
include $(WIFI_NEXT_PATH)/wifi.mk

# Stock BatteryMonitor-backed HAL. No fixed capacity/charging data or shutdown
# override; full testing requires demonstrated kernel SOC/status telemetry.
PRODUCT_PACKAGES += \
    android.hardware.health-service.example \
    t380.software-keymint-service \
    android.hardware.gatekeeper@1.0-service.software
# Software-only KeyMint reports SecurityLevel::SOFTWARE; no hardware_keystore
# feature XML is installed. Hardware-backed key storage is not demonstrated.
# Upstream software Gatekeeper supports credential-backed FBE; it does not
# provide hardware key isolation or durable hardware throttling.

PRODUCT_NAME := lineage_gta2swifi
PRODUCT_DEVICE := gta2swifi
PRODUCT_BRAND := samsung
PRODUCT_MANUFACTURER := samsung
PRODUCT_MODEL := SM-T380
PRODUCT_CHARACTERISTICS := tablet

# Diagnostic silent output; no microphone or physical audio feature claim.
include device/samsung/gta2swifi/audio/product.mk

# Proven legacy allocation contract for this pinned Adreno graphics stack.
PRODUCT_VENDOR_PROPERTIES += vendor.gralloc.disable_ahardware_buffer=1

# Stock software widget service used by the packaged tablet launcher.
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.software.app_widgets.xml:$(TARGET_COPY_OUT_SYSTEM)/etc/permissions/android.software.app_widgets.xml

# Credential support provided by the existing software Gatekeeper service.
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.software.secure_lock_screen.xml:$(TARGET_COPY_OUT_SYSTEM)/etc/permissions/android.software.secure_lock_screen.xml

# Temporary diagnostic awake guard; remove before deployment/suspend acceptance.
# System init executes the write using existing init wake-lock permissions.
PRODUCT_COPY_FILES += \
    device/samsung/gta2swifi/rootdir/t380-development-awake.rc:$(TARGET_COPY_OUT_SYSTEM)/etc/init/t380-development-awake.rc

# Standard compressed RAM swap, full target only; no disk writeback.
PRODUCT_COPY_FILES += \
    device/samsung/gta2swifi/rootdir/fstab.t380-zram:$(TARGET_COPY_OUT_VENDOR)/etc/fstab.t380-zram \
    device/samsung/gta2swifi/rootdir/t380-zram.rc:$(TARGET_COPY_OUT_SYSTEM)/etc/init/t380-zram.rc

# Keep Lineage's authenticated, non-debuggable userdebug default. Build the
# deployment candidate as a plain user variant after power/suspend acceptance.

# Standard ART ahead-of-time policy; native scrolling measured on Power052.
# Settings APK/source stays unchanged. See ART-PLATFORM-COMPILATION.md.
PRODUCT_DEXPREOPT_SPEED_APPS += Settings
