# SPDX-License-Identifier: Apache-2.0
DEVICE_PATH := device/samsung/gta2swifi
TARGET_ARCH := arm64
TARGET_ARCH_VARIANT := armv8-a
TARGET_CPU_ABI := arm64-v8a
TARGET_CPU_VARIANT := cortex-a53
TARGET_2ND_ARCH := arm
TARGET_2ND_ARCH_VARIANT := armv8-a
TARGET_2ND_CPU_ABI := armeabi-v7a
TARGET_2ND_CPU_ABI2 := armeabi
TARGET_2ND_CPU_VARIANT := cortex-a53

TARGET_BOARD_PLATFORM := msm8937
TARGET_ENFORCES_QSSI := true
BOARD_USES_QCOM_HARDWARE := true
BOARD_VENDOR := samsung
TARGET_BOOTLOADER_BOARD_NAME := SRPQD14A003
TARGET_NO_BOOTLOADER := true
TARGET_NO_RECOVERY := true
TARGET_OTA_ASSERT_DEVICE := gta2swifi

# Extract only hash-pinned successful diagnostics; never consume kernel output.
# Native shell preserves008. Full Wi-Fi selects reviewed025; the earlier
# graphics-only product remains014. Profile077 uses autonomous Samsung charging.
ifeq ($(T380_ENABLE_QCOM_GRAPHICS),true)
ifeq ($(T380_ENABLE_WIFI),true)
T380_KERNEL_DIAGNOSTIC := 077
else
T380_KERNEL_DIAGNOSTIC := 014
endif
else
T380_KERNEL_DIAGNOSTIC := 008
endif
TARGET_PREBUILT_KERNEL := $(DEVICE_PATH)/prebuilts/$(T380_KERNEL_DIAGNOSTIC)/kernel
TARGET_KERNEL_VERSION := 4.19
# Pinned immutable source supplies exported UAPI only; boot remains prebuilt.
TARGET_KERNEL_SOURCE := kernel/samsung/gta2swifi
# Lineage requires a base config name when source exists, even for UAPI-only
# exports. No defconfig is run: force the immutable diagnostic boot payload.
TARGET_KERNEL_CONFIG := vendor/msm8937-perf_defconfig
TARGET_FORCE_PREBUILT_KERNEL := true
BOARD_KERNEL_IMAGE_NAME := Image.gz-dtb
BOARD_KERNEL_BASE := 0x80000000
BOARD_KERNEL_PAGESIZE := 2048
BOARD_BOOT_HEADER_VERSION := 0
BOARD_MKBOOTIMG_ARGS := --kernel_offset 0x00080000 --ramdisk_offset 0x03600000 --tags_offset 0x03400000
BOARD_KERNEL_CMDLINE := console=null androidboot.hardware=qcom androidboot.bootdevice=7824900.sdhci androidboot.boot_devices=soc/7824900.sdhci lpm_levels.sleep_disabled=0 printk.devkmsg=on
ifeq ($(T380_ENABLE_QCOM_GRAPHICS),true)
BOARD_KERNEL_CMDLINE += mdss_mdp.panel=1:dsi:0:ss_dsi_panel_S6D7AA0X11_TV080WXM_WXGA:1:none:cfg:single_dsi
endif
BOARD_CUSTOM_BOOTIMG_MK := $(DEVICE_PATH)/bootimg.mk
# Preserve this build's Samsung QCDTv3 boot in both target-files and signing.
# Pinned build/make consumes BOARD_CUSTOM_BOOTIMG only as a presence flag,
# copying INSTALLED_BOOTIMAGE_TARGET into BOOTABLE_IMAGES for releasetools.
# COPY also establishes the boot build dependency and seeds IMAGES/boot.img;
# no diagnostic image hash or old ramdisk is selected here.
BOARD_CUSTOM_BOOTIMG := true
BOARD_COPY_BOOT_IMAGE_TO_TARGET_FILES := true

# Captured PIT, SYSTEM7176192 and BOOT65536 sectors at512bytes. No new
# partition geometry, super, metadata, vendor_boot or recovery image.
BOARD_BOOTIMAGE_PARTITION_SIZE := 33554432
BOARD_SYSTEMIMAGE_PARTITION_SIZE := 3674210304
BOARD_SYSTEMIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_FLASH_BLOCK_SIZE := 131072
TARGET_USERIMAGES_USE_EXT4 := true
TARGET_COPY_OUT_VENDOR := system/vendor
TARGET_COPY_OUT_PRODUCT := system/product
TARGET_COPY_OUT_SYSTEM_EXT := system/system_ext
TARGET_COPY_OUT_ODM := system/vendor/odm
BOARD_AVB_ENABLE := false
AB_OTA_UPDATER := false
BOARD_VNDK_VERSION := current
BOARD_SHIPPING_API_LEVEL := 25

ifeq ($(T380_ENABLE_QCOM_GRAPHICS),true)
DEVICE_MANIFEST_FILE := $(DEVICE_PATH)/manifest.xml $(DEVICE_PATH)/manifest-graphics-runtime.xml $(DEVICE_PATH)/audio/t380-audio-stub.xml
else
DEVICE_MANIFEST_FILE := $(DEVICE_PATH)/manifest-shell.xml
endif
TARGET_PRODUCT_PROP += $(DEVICE_PATH)/product.prop
TARGET_VENDOR_PROP += $(DEVICE_PATH)/vendor.prop
TARGET_SCREEN_DENSITY := 189

# Samsung legacy bootloader uses the kernel recovery restart route, with no
# Android misc/BCB partition. This guarded platform-init adaptation is T380 only.
$(call soong_config_set,t380_init,recovery_reboot_without_bcb,true)

# Bind the standard AIDL LAUNCH/INTERACTION calls to the narrow board policy.
ifeq ($(T380_ENABLE_QCOM_GRAPHICS),true)
$(call soong_config_set,qtipower,policy_ext_lib,libt380_power_policy)
endif

include device/qcom/sepolicy-legacy-um/SEPolicy.mk
# Dedicated household-charge sysfs access for the platform-signed schedule app.
SYSTEM_EXT_PRIVATE_SEPOLICY_DIRS += $(DEVICE_PATH)/sepolicy/system_ext/private
# Full-only SOFTWARE KeyMint executable label and observed battery targets.
# Existing domain permissions apply; this directory adds no allow rules.
ifeq ($(T380_ENABLE_QCOM_GRAPHICS),true)
BOARD_VENDOR_SEPOLICY_DIRS += $(DEVICE_PATH)/sepolicy/vendor
# TimeKeep domains/property and the exact PM8937 RTC label.
include hardware/sony/timekeep/sepolicy/SEPolicy.mk
ifeq ($(T380_ENABLE_WIFI),true)
include $(DEVICE_PATH)/wifi/BoardConfig-wifi.mk
BOARD_VENDOR_SEPOLICY_DIRS += $(DEVICE_PATH)/wifi/sepolicy
endif
endif

TARGET_USES_ION := true
TARGET_USES_GRALLOC1 := true
TARGET_USES_HWC2 := true
BOARD_USES_ADRENO := true
# Full Android diagnostic only. This is a directory, not a new partition.
ifeq ($(T380_ENABLE_QCOM_GRAPHICS),true)
BOARD_ROOT_EXTRA_FOLDERS += metadata
endif
