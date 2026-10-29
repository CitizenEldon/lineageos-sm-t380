# SPDX-License-Identifier: Apache-2.0
#requires -Version 7.3
param([Parameter(Mandatory)][string]$Manifest,[Parameter(Mandatory)][string]$Signature,[Parameter(Mandatory)][string]$PublicKey)
$ErrorActionPreference='Stop'
$key=[IO.File]::ReadAllBytes($PublicKey)
if ([Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($key)).ToLowerInvariant() -cne 'e20b6fad0fbf5f84ad108e6324763717105b84dd8c036ece0cf6280cf1e9f30e') { throw 'Unexpected public key' }
$rsa=[Security.Cryptography.RSA]::Create()
try {
    $rsa.ImportFromPem([Text.Encoding]::UTF8.GetString($key))
    if ($rsa.KeySize -ne 4096 -or -not $rsa.VerifyData([IO.File]::ReadAllBytes($Manifest),[IO.File]::ReadAllBytes($Signature),[Security.Cryptography.HashAlgorithmName]::SHA256,[Security.Cryptography.RSASignaturePadding]::Pkcs1)) { throw 'Signature verification failed' }
    'Signature verified'
} finally { $rsa.Dispose() }
