# Installation and recovery

This guide installs version 0.01 on the SM-T380 Wi-Fi model. Back up your personal data and have a way to restore Samsung firmware before you begin.

## Before you start

The installer expects the supplied rescue recovery to be installed and the data partition to be prepared for this Android build. It does not unlock the bootloader, install recovery, or migrate data from Samsung firmware. If you're starting from stock, those steps need to be completed first using a procedure for the SM-T380.

The tested device is a Rev05 SM-T380 with 16 GB storage. The installer checks the model, partition layout, and recovery image before making changes. The technical requirements are listed at the end of this guide.

Use Python 3.12+, [PowerShell 7.3 or newer](https://github.com/PowerShell/PowerShell), and [Android platform-tools](https://developer.android.com/tools/releases/platform-tools). OpenSSL is optional when PowerShell is on PATH. Unzip `lineageos-sm-t380-0.01.zip` into a writable folder and open a terminal there. Allow about 10 GB for the kit, unpacked images, and working files.

## 1. Verify the download

These commands run on your computer without contacting a tablet:

```sh
python3 -B tools/verify.py
python3 -B tools/unpack.py unpacked
pwsh -NoProfile -File tools/install.ps1 -Bundle unpacked -LastAcceptedSequence 0 -AuthenticateOnly
```

On Windows, use `python` or `py -3` instead of `python3`. The unpack destination should be a new folder; the tool creates it for you.

Verification checks the signature, exact image lengths, hashes, image formats, and source receipt. The signing-key fingerprint embedded in the tools is:

```text
e20b6fad0fbf5f84ad108e6324763717105b84dd8c036ece0cf6280cf1e9f30e
```

Use a trusted copy of the project to check this fingerprint. Download checksums detect transfer damage; the signature authenticates the OS images.

### About the installation counter

The public version is 0.01. Its signed images retain an internal sequence number of `36`, which the installer uses to prevent repeated or older installations. This counter is separate from the public version number.

`LastAcceptedSequence` is the highest sequence you have already accepted on your tablet. Use `0` for a tablet that has never accepted a build from this signing identity. Otherwise, use the value in your device's installation record. A value of `36` or higher means this image is already accepted or older; the installer will stop. Keep that record between downloads so the check continues to work.

## 2. Check your tablet

Boot into the supplied rescue recovery and connect the tablet by USB. In a PowerShell terminal:

```powershell
adb devices -l
$targetSerial = Read-Host 'Serial number of your SM-T380'
$lastAccepted = [long](Read-Host 'Previously accepted sequence for this tablet (0 for first installation)')
pwsh -NoProfile -File tools/install.ps1 -Bundle unpacked -Serial $targetSerial -LastAcceptedSequence $lastAccepted -CheckDevice
```

`CheckDevice` reads the tablet's identity, partition layout, and recovery image without changing them. It does not inspect encrypted user data. If a check fails, review the reported difference before proceeding; the checks protect against writing to incompatible hardware.

## 3. Install

Charge the tablet beforehand, keep the computer powered, and turn off the computer's automatic sleep during installation. Once your backups, recovery, and data partition are ready, run this in the same PowerShell terminal:

```powershell
pwsh -NoProfile -File tools/install.ps1 -Bundle unpacked -Serial $targetSerial -LastAcceptedSequence $lastAccepted -Execute -PrepareRecovery -ConfirmInstall -ConfirmSerial $targetSerial
```

`ConfirmInstall` confirms the write, and `ConfirmSerial` identifies the tablet. `PrepareRecovery` temporarily inhibits charging, stops the recovery interface, and unmounts recognized internal filesystems after the image and device checks pass. The recovery screen may stop responding at this point; that is expected.

The installer writes SYSTEM in verified chunks and reads it back before writing BOOT. Both images are checked over their exact signed byte lengths. Full readback takes time, so keep the USB connection in place until it finishes. DATA, EFS, modem, and bootloader partitions are left in place.

Without `Execute`, the command checks installation readiness. Without `PrepareRecovery`, recovery needs to be prepared manually. `CheckDevice` and `AuthenticateOnly` are separate read-only modes.

## 4. Finish and reboot

Keep the receipt folder printed by the installer. Both `system-final-verification.json` and `boot-final-verification.json` should show `verified: true`, with matching byte counts and hashes.

After both checks pass, reboot with:

```powershell
adb -s $targetSerial reboot
```

Check that Android starts and the functions you need work. The [feature table](../README.md#what-works) lists current support. After these checks, record accepted sequence `36` in your tablet's installation record. The installer leaves this final acceptance and the reboot to you.

## If installation stops

Keep the tablet in recovery with power and USB connected. Save `logs/` and inspect the failure receipts before rebooting or trying another write. A hash mismatch means the written bytes differ from the signed image and needs investigation.

For a completed SYSTEM write that needs further verification, `ResumeSystemReadback` can read and authenticate the entire existing SYSTEM image before deciding whether rewriting is necessary. It requires the same execution confirmations as an installation and is intended for recovery after the failure has been understood.

## Technical requirements

- Device: Samsung SM-T380, `gta2swifi`, platform `msm8937`.
- SYSTEM: `mmcblk0p45`, 3,674,210,304 bytes. BOOT: `mmcblk0p23`, 33,554,432 bytes. DATA: `mmcblk0p48`, 11,358,154,752 bytes. Both partition-alias trees need to agree.
- Recovery: the supplied TWRP `3.7.0_9-0` image with a root shell. Its 33,554,432-byte recovery partition has SHA-256 `b1fa28acd454d145e0f650b216c36e3f361cd91988e97f46f65ea4fe923147fb`.
- DATA: an existing filesystem compatible with this build's ext4/FBEv2 configuration. The installer does not format or migrate it.

`release/rescue-recovery.img.gz` contains the recovery image. It omits DATA from its mount list and does not prepare or repair that partition. Recovery setup requires the correct SM-T380 partition mapping. Component source information is in [LICENSING.md](LICENSING.md).
