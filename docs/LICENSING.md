# Licenses and provenance

This page records component sources and available license information. Local preparation assumes redistribution permission; applicable grants and notices belong with the release records.

## Open-source material

New distribution tools marked `SPDX-License-Identifier: Apache-2.0` use the included Apache 2.0 license. Upstream files and patches retain their respective licensing. The full kernel archive includes its COPYING and LICENSES files, complete source, and retained configuration. The complete Android revision manifest and custom source are under `source/`.

This repository's license does not relicense the compiled OS, proprietary libraries, or firmware. Existing notices embedded in the OS remain part of the unchanged SYSTEM image.

## Graphics firmware

`a300_pfp.fw` and `a300_pm4.fw` were originally retained from Samsung firmware. Their SHA-256 hashes exactly match the Qualcomm files in linux-firmware tag `20250211`:

| File | SHA-256 |
| --- | --- |
| a300_pfp.fw | b6e66400d46d52dc343d370b94362d564a765994bda47dc026961dd47a9ca7a9 |
| a300_pm4.fw | f78c57e879e96a2c0ceeea31dbd30f7f1d4526f9c876e80481c654f08daaecd3 |

The upstream WHENCE file assigns these files to Qualcomm's redistribution agreement and notice. The complete texts are supplied in `licenses/Qualcomm-firmware.txt` and `licenses/Qualcomm-NOTICE.txt`; read those terms, including the Qualcomm-chipset and binary-distribution conditions.

- [Upstream license mapping](https://kernel.googlesource.com/pub/scm/linux/kernel/git/firmware/linux-firmware.git/+/refs/tags/20250211/WHENCE)
- [Qualcomm agreement](https://kernel.googlesource.com/pub/scm/linux/kernel/git/firmware/linux-firmware.git/+/refs/tags/20250211/LICENSE.qcom)
- [Accompanying notices](https://kernel.googlesource.com/pub/scm/linux/kernel/git/firmware/linux-firmware.git/+/refs/tags/20250211/qcom/NOTICE.txt)

## Graphics libraries

The 18 Adreno/display shared libraries are byte-identical to [TheMuppets donor revision e264bd19](https://github.com/TheMuppets/proprietary_vendor_xiaomi_mithorium-common/tree/e264bd19f6158e9882efa1d7afe28e34bfd8ee4d). The [LineageOS donor manifest](https://github.com/LineageOS/android_device_xiaomi_mithorium-common/blob/2639cd7377cbd6b6ecf6f5317637c3faca96f86c/proprietary-files-qc-vndr.txt) attributes them to `Nokia/Panther_00WW/PAN_sprout:11/RKQ1.200928.002/00WW_3_240:user/release-keys`.

The inspected donor repository supplied no license/notice file. The firmware agreement above is not assumed to cover these libraries. `source/overlay/vendor/samsung/gta2swifi/NOTICE` is the project's provenance note, not a Qualcomm permission grant. Exact library hashes and donor receipts are preserved beside it.

## Other binary inputs and rescue recovery

The graphics investigation did not establish terms for every other binary in the OS. The Wi-Fi payload inventory is in `source/overlay/device/samsung/gta2swifi/wifi/payload-manifest.json`; all recovered binary inputs are listed in `source/inputs.json`.

The rescue image is a modified TWRP `3.7.0_9-0` build with DATA omitted from its fstab and a project rescue script. The kit includes it for local preparation. Its complete build-source provenance still needs to be documented before distribution. The kernel archive supplied here corresponds to the Android OS; the recovery uses a separate kernel.
