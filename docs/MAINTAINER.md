# Maintaining a release

Repository: `lineageos-sm-t380`

Description: Unofficial LineageOS 23.2 / Android 16 for the Samsung Galaxy Tab A 8.0 (SM-T380), with installation tools and source.

Topics: `android`, `lineageos`, `samsung`, `sm-t380`, `gta2swifi`, `custom-rom`.

## Versioning

The public version starts at **0.01**. Use tag `v0.01` and release title `LineageOS for SM-T380 0.01`.

Public version numbers and signing sequences serve different purposes. Version 0.01 packages the existing signed images, whose internal sequence is `36`. Keep their manifest and signature unchanged. Renaming or repackaging a download does not require a new signing sequence. A new OS build uses the original publisher's private signing workflow and the next sequence from its authoritative ledger.

## Verify and package

Run from the public repository root:

```sh
python3 -B tools/update-inventory.py
python3 -B tools/verify.py
python3 -B -m unittest discover -s tools/tests -p 'test_*.py' -v
pwsh -NoProfile -File tools/tests/test_installer.ps1
pwsh -NoProfile -File tools/tests/test_public_installer.ps1
python3 -B tools/package-release.py
```

Review newly added files before refreshing the inventory. `update-inventory.py` records public Git-visible files and checks for private keys, backups, and oversized files. `package-release.py` includes that inventory and the three named release assets. It creates local files without uploading them.

The output in `dist/` is:

| File | Contents |
| --- | --- |
| `lineageos-sm-t380-0.01.zip` | OS images, installer, documentation, and source |
| `kernel-source-0.01.tar.gz` | Matching complete kernel source, also included in the kit |
| `SHA256SUMS` | Checksums for both downloads |

Large archives, installation logs, unpacked images, and `dist/` are excluded from Git. Publish the repository inside `github/lineageos-sm-t380/`; the parent project contains private archival material.

## Publish on GitHub

Create an empty `lineageos-sm-t380` repository using the description above. The local repository is on `main`, with files staged and no remote configured. Review the staged changes, make the initial commit with your public Git author identity, and push the repository.

Let the supplied CI workflow run and review its results. Create a release tagged `v0.01`, use [RELEASE-NOTES.md](RELEASE-NOTES.md) for its description, and attach the three files listed above. The automatic GitHub source downloads supplement the release kit.

Before distributing binaries, retain the applicable redistribution grants and notices and complete the custom recovery's source provenance described in [LICENSING.md](LICENSING.md). The matching Android kernel archive covers the OS kernel; the recovery has a separate kernel.

The installation guide starts with recovery and DATA already prepared. A future stock-to-Lineage guide should include the SM-T380 recovery setup, data preparation, and a tested rollback route.

Keep private signing identities and the authoritative signing ledger outside this repository. Review the final ZIP and staged files for personal data before publishing.
