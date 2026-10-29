# Staging example. WIFI_NEXT_PATH must point to the integrated, reviewed tree.
PRODUCT_PACKAGES += android.hardware.wifi-service libcld80211 libwifi-hal-ctrl \
    libwpa_client hostapd wificond wpa_supplicant wpa_supplicant.conf t380-wifi-prepare t380-wifi-stage
PRODUCT_VENDOR_PROPERTIES += wifi.interface=wlan0
# WifiCountryCode reads this actual stock property, not a guessed resource.
PRODUCT_SYSTEM_PROPERTIES += ro.boot.wificountrycode=US
PRODUCT_COPY_FILES += $(WIFI_NEXT_PATH)/t380-wifi.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/t380-wifi.rc
PRODUCT_COPY_FILES += frameworks/native/data/etc/android.hardware.wifi.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.wifi.xml
include $(WIFI_NEXT_PATH)/firmware.mk
# Firmware/module copies are generated exclusively from payload-manifest.json.
# Exact paths are vendor/firmware/wcnss.*, vendor/firmware/wlan/prima/*,
# vendor/firmware/regulatory.db{,.p7s}, vendor/lib/modules/wlan.ko.

# STA-only supplicant mapping; preserve stock seed PMF/RSN configuration.
PRODUCT_COPY_FILES += $(WIFI_NEXT_PATH)/wpa_supplicant_overlay.conf:$(TARGET_COPY_OUT_VENDOR)/etc/wifi/wpa_supplicant_overlay.conf

# Firmware PNO stays disabled; opt into stock software PNO scan fallback.
# Provision DeviceConfig wifi/software_pno_enabled separately and verify it.
PRODUCT_PACKAGES += T380WifiResourcesOverlay
