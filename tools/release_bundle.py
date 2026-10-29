#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Authenticate the signed OS images; optional extraction never contacts a device."""
from pathlib import Path
import hashlib
import json
import re
import shutil
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PUBLIC_KEY_SHA256 = 'e20b6fad0fbf5f84ad108e6324763717105b84dd8c036ece0cf6280cf1e9f30e'
LIMITS = {'manifest.json': 16384, 'manifest.sig': 512, 'source-receipt.json': 32768,
          'boot.img': 33554432, 'system.img': 3674210304}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, 'Duplicate JSON property: '+key)
        result[key] = value
    return result


def read_json(data):
    return json.loads(data, object_pairs_hook=unique_object)


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def verify_signature(metadata, key):
    require(sha(key) == PUBLIC_KEY_SHA256, 'Public signing key differs from the embedded trust pin')
    with tempfile.TemporaryDirectory(prefix='t380-signature-') as tmp:
        folder = Path(tmp)
        for name in ('manifest.json', 'manifest.sig'):
            (folder/name).write_bytes(metadata[name])
        openssl = shutil.which('openssl')
        if openssl:
            command = [openssl, 'dgst', '-sha256', '-verify', str(key), '-signature',
                       str(folder/'manifest.sig'), str(folder/'manifest.json')]
        else:
            pwsh = shutil.which('pwsh')
            require(pwsh, 'Install OpenSSL or PowerShell 7.3+ for signature verification')
            command = [pwsh, '-NoProfile', '-File', str(ROOT/'tools/verify-signature.ps1'),
                       '-Manifest', str(folder/'manifest.json'), '-Signature', str(folder/'manifest.sig'),
                       '-PublicKey', str(key)]
        result = subprocess.run(command, capture_output=True, text=True)
        require(result.returncode == 0, 'Distribution signature verification failed: '+result.stderr.strip())


def verify_bundle(archive, destination=None, key=None):
    key = Path(key) if key else ROOT/'trust/distribution.public.pem'
    metadata, images, seen = {}, {}, set()
    created = False
    if destination is not None:
        destination = Path(destination).absolute()
        # Never mix authenticated files with an existing directory, including symlinks.
        destination.mkdir(parents=True, exist_ok=False)
        created = True
    try:
        with tarfile.open(archive, 'r|gz') as bundle:
            for member in bundle:
                name = member.name
                require(name in LIMITS and name not in seen and member.isfile(), 'Unexpected/duplicate archive entry: '+name)
                require(0 < member.size <= LIMITS[name], 'Invalid archived length: '+name)
                seen.add(name)
                digest, count, prefix = hashlib.sha256(), 0, b''
                stream = bundle.extractfile(member)
                output = (destination/name).open('xb') if destination is not None else None
                small = bytearray()
                try:
                    while block := stream.read(1024*1024):
                        count += len(block)
                        require(count <= LIMITS[name], 'Archive stream exceeds bounds')
                        digest.update(block)
                        if len(prefix) < 1082:
                            prefix = (prefix+block)[:1082]
                        if name.endswith('.img'):
                            pass
                        else:
                            small.extend(block)
                        if output:
                            output.write(block)
                finally:
                    if output:
                        output.close()
                require(count == member.size, 'Truncated archive entry: '+name)
                if name.endswith('.img'):
                    images[name] = {'bytes': count, 'sha256': digest.hexdigest()}
                    if name == 'system.img':
                        require(count == LIMITS[name] and prefix[1080:1082] == b'\x53\xef', 'SYSTEM is not the expected raw ext4 image')
                    else:
                        require(prefix[:8] == b'ANDROID!', 'BOOT has invalid magic')
                else:
                    metadata[name] = bytes(small)
        require(seen == set(LIMITS), 'Release bundle is incomplete')
        verify_signature(metadata, key)
        manifest = read_json(metadata['manifest.json'])
        require(type(manifest.get('schema')) is int and manifest['schema'] == 1, 'Invalid manifest schema')
        require(type(manifest.get('sequence')) is int and manifest['sequence'] == 36, 'Expected release sequence 36')
        require(manifest.get('device') == 'gta2swifi', 'Wrong device in signed manifest')
        require(set(manifest.get('images', {})) == {'boot.img','system.img'}, 'Invalid image set')
        for name, value in manifest['images'].items():
            require(type(value.get('bytes')) is int and value['bytes'] > 0, 'Invalid signed byte count')
            require(isinstance(value.get('sha256'), str) and re.fullmatch('[0-9a-f]{64}', value['sha256']), 'Invalid signed digest')
        require(manifest['images'] == images, 'Images differ from exact signed sizes or digests')
        require(manifest.get('source_receipt_sha256') == hashlib.sha256(metadata['source-receipt.json']).hexdigest(), 'Source receipt differs from signed digest')
        read_json(metadata['source-receipt.json'])
        return {'status': 'PASS', 'sequence': 36, 'device': 'gta2swifi', 'images': images,
                'signatureVerified': True, 'sourceReceiptVerified': True, 'deviceActions': False}
    except BaseException:
        if created:
            shutil.rmtree(destination)
        raise
