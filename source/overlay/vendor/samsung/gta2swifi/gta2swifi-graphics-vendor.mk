# SPDX-License-Identifier: Apache-2.0
# Opt-in only: inherit from the full graphics product, never the native shell.
PRODUCT_SOONG_NAMESPACES += vendor/samsung/gta2swifi

PRODUCT_PACKAGES += \
    eglSubDriverAndroid \
    libEGL_adreno \
    libGLESv1_CM_adreno \
    libGLESv2_adreno \
    libgsl \
    libadreno_utils \
    libllvm-glnext \
    libCB \
    libsdmextension \
    libscalar \
    t380-lib-libEGL_adreno-root-link \
    t380-lib-libGLESv2_adreno-root-link \
    t380-lib64-libEGL_adreno-root-link \
    t380-lib64-libGLESv2_adreno-root-link \
    t380-a300_pfp.fw \
    t380-a300_pm4.fw
