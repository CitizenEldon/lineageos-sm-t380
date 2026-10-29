# Source and rebuilding

## Reuse the signed release

Version 0.01 includes the existing signed BOOT and SYSTEM images, manifest, signature, and source receipt. Their internal signing sequence is unchanged; see the [versioning notes](MAINTAINER.md#versioning).

`tools/unpack.py` authenticates and extracts those files. `source/inputs.json` maps 41 binary inputs to exact locations inside the signed images, including the kernel, DT, WLAN module, proprietary firmware/libraries, and a WebView reconstruction delta.

## Complete kernel source

`kernel-source-0.01.tar.gz` contains the complete matching kernel tree, configuration, GPL notices, and assembly receipt. Its 79,547-file tree is checked against the retained fingerprint:

```text
35ddbc2bef63981a11d01e5e15e11be46ce3c1c404656d5a1318a1142ffc6010
```

This fingerprint includes content hashes, executable status, and exact symlink text. It is not the compressed archive's checksum; the latter is listed in `release/assets.json`.

To assemble the complete source again, without compiling:

```sh
python3 -B tools/prepare-kernel-source.py /absolute/new-case-sensitive-workspace --output /absolute/kernel-source-0.01.tar.gz
```

It downloads the pinned upstream kernel, applies the retained patch and integration files, verifies the entire tree, and creates the archive. Use `--upstream-archive` to reuse a previously downloaded source tarball. The final tree fingerprint is always checked.

Linux filesystems normally preserve case. Default macOS volumes do not: this kernel contains 13 pairs of filenames distinguished only by capitalization. On macOS, create a case-sensitive APFS disk image in Disk Utility, mount it, and put the workspace there. The tool detects and rejects a case-insensitive workspace before extraction. Packaging a previously prepared archive needs no special volume.

## Reconstruct Android sources

Use Linux x86-64, Python 3.12+, Git/repo/Git LFS, normal LineageOS build dependencies, `debugfs` from e2fsprogs, and ARM GNU cross-compilers. Use a case-sensitive filesystem. The entire Android checkout and build are much larger than the release kit.

From the repository root, with absolute paths and a new empty build workspace:

```sh
PROJECT="$PWD"
WORK=/absolute/new-build-workspace
mkdir -p "$WORK/android"
cd "$WORK/android"
repo init -u https://github.com/LineageOS/android -b lineage-23.2 --git-lfs
cp "$PROJECT/source/android.xml" .repo/manifests/t380.xml
repo init -m t380.xml
repo sync -c --no-tags -j8 --fail-fast
cd "$PROJECT"
python3 -B tools/develop.py source "$WORK"
bash tools/build-android.sh "$WORK"
```

The source reconstruction uses frozen `source/overlay.tar.gz`. `source/overlay/` is an identical readable view for GitHub; verification checks that the two agree. Interpreter caches are excluded.

The public snapshot includes the complete pinned upstream manifest and custom changes. It is not an offline mirror of every Android repository. The historical signed source receipt mentions older intermediate archives that were superseded during consolidation; current reconstruction uses the files documented here and validates the retained kernel/UAPI fingerprints.

For kernel development, after source reconstruction:

```sh
python3 -B tools/develop.py kernel "$WORK"
```

The recipe uses the pinned `clang-r563880c` Linux toolchain, ARM cross-compilers, retained configuration, and source epoch. Review and stage the new kernel, Rev05 DT/QCDT, and stripped WLAN module together. Update their device-tree receipts and approved hashes before compiling Android. The kernel and WLAN module need to match.

## Signing a new build

The public repository contains only the distribution public key. It contains no original private signing identities, encrypted signing backups, release-issuance ledger, or private builder configuration.

Independent builders use their own signing identities and distribution policy. A build signed with different keys may require a fresh installation. For releases from the original publisher, use the separately retained signing workflow, keep the existing keys, and reserve a sequence above the live ledger's highest issued value.

Local preparation verifies the existing images and reconstructs the complete kernel source. Full Android compilation runs separately on Linux.
