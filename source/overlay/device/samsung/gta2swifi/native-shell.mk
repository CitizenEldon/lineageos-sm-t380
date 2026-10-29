# SPDX-License-Identifier: Apache-2.0
# Android16 base.mk also installs the Java framework and UI. This bounded native
# product instead selects the system pieces needed to mount DATA, activate the
# bionic/adbd APEXes and expose the Android shell on the proven USB controller.
# Match the scan exclusions at vendor/lineage/config/common.mk:4-6 without
# inheriting that file's framework/application package additions.
PRODUCT_SOURCE_ROOT_DIRS += -kernel/platform -prebuilts/misc/protobuf_vendorcompat

PRODUCT_PACKAGES += \
    adbd_system_api \
    apexd \
    blkid \
    boringssl_self_test \
    bpfloader \
    build_flag_system \
    build_flag_system_ext \
    build_flag_vendor \
    cgroups.json \
    com.android.adbd \
    com.android.i18n \
    com.android.runtime \
    debuggerd \
    dmctl \
    e2fsck \
    fs_config_dirs_nonsystem \
    fs_config_dirs_system \
    fs_config_dirs_system_ext \
    fs_config_files_nonsystem \
    fs_config_files_system \
    fs_config_files_system_ext \
    group_system \
    group_system_ext \
    group_vendor \
    hwservicemanager \
    init.environ.rc \
    init_system \
    init_vendor \
    init.usb.rc \
    init.usb.configfs.rc \
    libc.bootstrap \
    libdl.bootstrap \
    libdl_android.bootstrap \
    libm.bootstrap \
    linker \
    logcat \
    logd \
    mke2fs \
    passwd_system \
    passwd_system_ext \
    passwd_vendor \
    product-build.prop \
    resize2fs \
    selinux_policy_nonsystem \
    selinux_policy_system \
    selinux_policy_system_ext \
    selinux_policy_vendor \
    servicemanager \
    sh \
    task_profiles.json \
    toolbox \
    toybox \
    tune2fs \
    vdc \
    vendor_compatibility_matrix.xml \
    vintf \
    vold \
    system-build.prop \
    system_ext-build.prop \
    system_manifest.xml \
    system_compatibility_matrix.xml \
    system_ext_manifest.xml

PRODUCT_HOST_PACKAGES += adb e2fsdroid mke2fs
PRODUCT_SYSTEM_PROPERTIES += debug.atrace.tags.enableflags=0
