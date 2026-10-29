# Full-product-only integration paired with approved diagnostic020.
BOARD_HAS_QCOM_WLAN := true
BOARD_HOSTAPD_DRIVER := NL80211
BOARD_HOSTAPD_PRIVATE_LIB := //hardware/qcom-caf/wlan/qcwcn:lib_driver_cmd_qcwcn
BOARD_WLAN_DEVICE := qcwcn
BOARD_WPA_SUPPLICANT_DRIVER := NL80211
BOARD_WPA_SUPPLICANT_PRIVATE_LIB := //hardware/qcom-caf/wlan/qcwcn:lib_driver_cmd_qcwcn
WIFI_HAL_INTERFACE_COMBINATIONS := {{{STA}, 1}}
WIFI_DRIVER_STATE_CTRL_PARAM := "/sys/module/wlan/parameters/fwpath"
WIFI_DRIVER_STATE_ON := ""
WIFI_DRIVER_STATE_OFF := ""
WIFI_AVOID_IFACE_RESET_MAC_CHANGE := true
WIFI_HIDL_UNIFIED_SUPPLICANT_SERVICE_RC_ENTRY := true
WPA_SUPPLICANT_VERSION := VER_0_8_X
# No WIFI_DRIVER_MODULE_PATH: helper owns the only boot-time finit_module.

# Supported kernel-module packaging preserves the exact reviewed ELF bytes.
# Empty modules.load prevents generic autoload ahead of real preparation.
BOARD_VENDOR_KERNEL_MODULES += $(DEVICE_PATH)/wifi/payload/vendor/lib/modules/wlan.ko
BOARD_DO_NOT_STRIP_VENDOR_MODULES := true
BOARD_VENDOR_KERNEL_MODULES_LOAD := false
