# SPDX-License-Identifier: Apache-2.0
# Standard Android v0 packer first; then proven Samsung legacy QCDTv3 framing.
T380_BOOT_TOOL := $(DEVICE_PATH)/tools/package-boot.py
T380_QCDT := $(dir $(TARGET_PREBUILT_KERNEL))dt.img
$(INSTALLED_BOOTIMAGE_TARGET): $(MKBOOTIMG) $(INSTALLED_KERNEL_TARGET) $(INTERNAL_BOOTIMAGE_FILES) $(T380_BOOT_TOOL) $(T380_QCDT) $(dir $(TARGET_PREBUILT_KERNEL))receipt.json $(DEVICE_PATH)/tools/approved-kernels.json
	$(MKBOOTIMG) --kernel $(INSTALLED_KERNEL_TARGET) $(INTERNAL_BOOTIMAGE_ARGS) $(INTERNAL_MKBOOTIMG_VERSION_ARGS) $(BOARD_MKBOOTIMG_ARGS) --output $@.android-v0
	python3 $(T380_BOOT_TOOL) $@.android-v0 $(T380_QCDT) $@
	$(call assert-max-image-size,$@,$(BOARD_BOOTIMAGE_PARTITION_SIZE))
