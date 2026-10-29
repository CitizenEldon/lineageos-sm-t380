#!/usr/bin/env bash
set -euo pipefail
workspace=$(cd "$1" && pwd)
cd "$workspace/android"
unset WITH_ADB_INSECURE WITH_SU SELINUX_IGNORE_NEVERALLOWS
export OUT_DIR="$workspace/android-out" LC_ALL=C TZ=UTC
set +u
source build/envsetup.sh
lunch lineage_gta2swifi bp4a user
test "$(get_build_var TARGET_BUILD_VARIANT)" = user
test "$(get_build_var SELINUX_IGNORE_NEVERALLOWS)" != true
m -j"${JOBS:-8}" target-files-package otatools
