# SPDX-License-Identifier: Apache-2.0
# Native Android16 userspace/ADB milestone. No Java/UI/GLES/blob requirement.
$(call inherit-product, device/samsung/gta2swifi/native-shell.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/product_launched_with_n_mr1.mk)
T380_ENABLE_QCOM_GRAPHICS := false
$(call inherit-product, device/samsung/gta2swifi/device.mk)
TARGET_SUPPORTS_32_BIT_APPS := true
TARGET_SUPPORTS_64_BIT_APPS := true
PRODUCT_NAME := lineage_gta2swifi_shell
PRODUCT_DEVICE := gta2swifi
PRODUCT_BRAND := samsung
PRODUCT_MANUFACTURER := samsung
PRODUCT_MODEL := SM-T380 Lineage23.2 shell
PRODUCT_CHARACTERISTICS := tablet
PRODUCT_SYSTEM_PROPERTIES += ro.t380.shell=1
# Diagnostic-only eng target, matching the temporary008 debugging trust model.
# SELinux is kept enforcing by default; coordinator may select a diagnostic
# permissive boot explicitly if real denial evidence requires it.
PRODUCT_SYSTEM_DEFAULT_PROPERTIES += ro.adb.secure=0

# Keep the source branch/version identity without inheriting common_mobile,
# which intentionally brings in the full Lineage framework and applications.
LINEAGE_BUILD := gta2swifi_shell
include vendor/lineage/config/version.mk
