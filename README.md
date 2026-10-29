# LineageOS for Samsung Galaxy Tab A 8.0 (SM-T380)

An unofficial LineageOS 23.2 build based on Android 16 for the Samsung Galaxy Tab A 8.0 (2017), Wi-Fi model SM-T380 (`gta2swifi`).

Version **0.01** brings a newer Android release to this tablet, with display, touch, Wi-Fi, and performance tuning for its hardware. The download includes the OS, installation tools, and source needed to build on the project.

## What works

| Feature | Status |
| --- | --- |
| Display, touch, and Wi-Fi | Working |
| Brightness and Settings | Working |
| Navigation | Performance tuned for the SM-T380's hardware |
| Charging and suspend | Working |
| Speaker and microphone | Not supported |
| Camera and Bluetooth | Untested |
| GApps compatibility | Untested |

Tested on an SM-T380 with a Rev05 board and 16 GB storage. Other hardware revisions and storage configurations are untested. This build is for the Wi-Fi model; LTE variants use different hardware.

## Download and install

Download `lineageos-sm-t380-0.01.zip` and `SHA256SUMS` from the project's GitHub Releases. The kit includes the matching kernel source, also available separately as `kernel-source-0.01.tar.gz`. GitHub's automatic "Source code" downloads contain the repository files only; choose the release kit for the OS images.

The [installation guide](docs/INSTALL.md) covers verification, device checks, installation, and recovery. It starts with the supplied rescue recovery already installed and a compatible Android data partition. If your tablet still runs Samsung firmware, recovery setup and data preparation are separate steps.

To verify and unpack the kit on your computer:

```sh
python3 -B tools/verify.py
python3 -B tools/unpack.py unpacked
```

You'll need Python 3.12+ and either OpenSSL or PowerShell 7.3+. On Windows, use `python` or `py -3` in place of `python3`. These commands run without a connected tablet. For a source-only checkout, use `python3 -B tools/verify.py --repository-only`.

The installer writes BOOT and SYSTEM. It leaves personal data, EFS, the bootloader, and modem partitions in place and does not format DATA. Back up your data before installing.

## Security

SELinux is enforcing. Key storage and credential checks run in software, without hardware-backed protection. Android Verified Boot, which checks OS integrity at startup, is disabled. The installer verifies signed images before writing them and checks the written bytes afterward.

## Source and documentation

- [Release notes](docs/RELEASE-NOTES.md)
- [Build instructions](docs/BUILDING.md)
- [Testing](docs/VALIDATION.md)
- [Licenses and component sources](docs/LICENSING.md)
- [Maintainer guide](docs/MAINTAINER.md)

The [Android manifest](source/android.xml) pins upstream versions. [Custom patches](source/patches) and [integration source](source/overlay) contain this project's changes. A full Android build needs Linux x86-64; the existing release can be verified and packaged on a Mac.

## Reporting a problem

Include your tablet model, previous OS, computer OS, and the command or steps that produced the problem. Installer records are saved under `logs/`; remove device serial numbers and personal paths before sharing them. Keep signing keys, device backups, and personal data private.

If installation verification fails, leave the tablet in recovery and keep the logs so the failure can be investigated.

This is an independent project, unaffiliated with Samsung or the LineageOS project. Component licenses are described in [LICENSE](LICENSE).
