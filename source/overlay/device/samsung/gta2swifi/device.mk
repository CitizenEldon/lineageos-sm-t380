# SPDX-License-Identifier: Apache-2.0
LOCAL_PATH := device/samsung/gta2swifi
$(call inherit-product, hardware/qcom-caf/common/common.mk)
$(call inherit-product, frameworks/native/build/tablet-7in-hdpi-1024-dalvik-heap.mk)

PRODUCT_AAPT_CONFIG := normal
PRODUCT_AAPT_PREF_CONFIG := hdpi
PRODUCT_SOONG_NAMESPACES += $(LOCAL_PATH)
PRODUCT_OTA_ENFORCE_VINTF_KERNEL_REQUIREMENTS := false
PRODUCT_SYSTEM_SERVER_COMPILER_FILTER := speed-profile

# Deliberately select only the source-built graphics path from the pinned23.2
# donor. The full product separately inherits the inventoried graphics package.
ifeq ($(T380_ENABLE_QCOM_GRAPHICS),true)
PRODUCT_PACKAGES += \
    android.hardware.graphics.allocator@2.0-impl \
    android.hardware.graphics.allocator@2.0-service \
    android.hardware.graphics.mapper@2.0-impl-2.1 \
    android.hardware.graphics.composer@2.1-service \
    android.hardware.power-service-qti \
    gralloc.msm8937 \
    hwcomposer.qcom
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/rootdir/ueventd-graphics.rc:$(TARGET_COPY_OUT_VENDOR)/ueventd.rc \
    $(LOCAL_PATH)/rootdir/init.t380-gpu-power.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.t380-gpu-power.rc
else
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/rootdir/ueventd.rc:$(TARGET_COPY_OUT_VENDOR)/ueventd.rc
endif

# Advertise the USB capabilities implemented by the accepted kernel so
# SystemServer starts UsbService on the secure user product.
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.usb.host.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.usb.host.xml \
    frameworks/native/data/etc/android.hardware.usb.accessory.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.usb.accessory.xml

PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/rootdir/fstab.qcom:$(TARGET_COPY_OUT_RAMDISK)/fstab.qcom \
    $(LOCAL_PATH)/rootdir/fstab.qcom:$(TARGET_COPY_OUT_VENDOR)/etc/fstab.qcom \
    $(LOCAL_PATH)/rootdir/init.qcom.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/hw/init.qcom.rc \
    $(LOCAL_PATH)/rootdir/t380-usb-adbd-lifecycle.rc:$(TARGET_COPY_OUT_SYSTEM)/etc/init/t380-usb-adbd-lifecycle.rc \
    $(LOCAL_PATH)/keylayout/gpio-keys.kl:$(TARGET_COPY_OUT_VENDOR)/usr/keylayout/gpio-keys.kl \
    $(LOCAL_PATH)/keylayout/sec_touchscreen.kl:$(TARGET_COPY_OUT_VENDOR)/usr/keylayout/sec_touchscreen.kl

# No Wi-Fi/audio/camera/radio capabilities or power-control service is selected
# here. The full product adds health monitoring; the kernel owns charging.
# Full Android diagnostic only: preserve the 00 prefix for init ordering.
ifeq ($(T380_ENABLE_QCOM_GRAPHICS),true)
PRODUCT_SYSTEM_PROPERTIES += ro.t380.volatile_metadata=1
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/rootdir/00-t380-metadata.rc:$(TARGET_COPY_OUT_SYSTEM)/etc/init/00-t380-metadata.rc
endif

# Dedicated local display controller; disabled until configured.
ifeq ($(T380_ENABLE_WIFI),true)
PRODUCT_PACKAGES += T380DisplaySchedule
endif

# T380 keyguard: preserve the touch target while using density-safe icon padding.
DEVICE_PACKAGE_OVERLAYS += $(LOCAL_PATH)/overlay
PRODUCT_ENFORCE_RRO_EXCLUDED_OVERLAYS += $(LOCAL_PATH)/overlay

# Connect framework and Quick Settings brightness to the existing panel driver.
ifeq ($(T380_ENABLE_QCOM_GRAPHICS),true)
PRODUCT_PACKAGES += android.hardware.lights-service.t380
endif
