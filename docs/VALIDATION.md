# Testing

## Device use

The build has been used on an SM-T380 Rev05 with 16 GB storage. Display, touch, Wi-Fi, brightness, and Settings work, and navigation has received performance tuning. The owner reports that charging and suspend work in everyday use.

Speaker and microphone support are absent. Camera, Bluetooth, and GApps compatibility are untested.

## Package and installer checks

Local checks on the M2 Mac cover:

- The original RSA4096/SHA-256 signature, source receipt, exact BOOT/SYSTEM lengths, and full image hashes.
- Offline authentication of the real OS images through the public PowerShell installer.
- Complete kernel source reconstruction: 79,547 entries match the recorded fingerprint, including file contents, executable flags, and symlinks.
- Seven Python test methods for signatures, changed keys, duplicate JSON, archive paths and entry types, and extraction failure. The archive test includes six malformed-entry cases.
- Twenty-three installer regression cases for exact-range hashing, byte-count normalization, verification records, fallback behavior, and hash mismatches.
- Twenty-two public-installer cases for device and recovery checks, incompatible command options, exact BOOT reads, changed image files, and incomplete or failed readback.
- Checks for private signing material, personal paths, and device identifiers in the public files.

The packaged files are checked against the repository inventory and asset hashes. Source and preparation records are in `release/`.

The automated tests run without a tablet. They exercise the installer and verification tools; they do not measure hardware performance. The supplied CI workflow covers macOS, Windows, and Linux and will run when the repository is pushed to GitHub. Local execution has been checked on macOS.
