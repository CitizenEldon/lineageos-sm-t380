# SPDX-License-Identifier: Apache-2.0
#requires -Version 7.3
param(
    [Parameter(Mandatory)][string]$Bundle,
    [ValidatePattern('^[A-Za-z0-9._:-]{1,128}$')][string]$Serial,
    [string]$AdbPath,
    [string]$TrustedPublicKey = (Join-Path $PSScriptRoot '../trust/distribution.public.pem'),
    [Parameter(Mandatory)][ValidateRange(0,9223372036854775807)][long]$LastAcceptedSequence,
    [switch]$AuthenticateOnly,
    [switch]$CheckDevice,
    [switch]$Execute,
    [switch]$PrepareRecovery,
    [Alias('AcknowledgeExperimentalBuild')][switch]$ConfirmInstall,
    [string]$ConfirmSerial,
    [switch]$ResumeSystemReadback,
    [string]$StagedHashHelper,
    [string]$StagedHashHelperSha256
)
# Version 0.01, signed sequence 36. No key generation, DATA formatting, automatic reboot,
# acceptance-ledger update, or replay of ambiguous writes.
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$TrustedPublicKeySha256 = 'e20b6fad0fbf5f84ad108e6324763717105b84dd8c036ece0cf6280cf1e9f30e'
$ExpectedIssuedSequence = 36L
if ($AuthenticateOnly -and ($Execute -or $PrepareRecovery -or $CheckDevice -or $ResumeSystemReadback)) { throw 'Offline authentication cannot request device actions' }
if ($CheckDevice -and ($Execute -or $PrepareRecovery -or $ResumeSystemReadback)) { throw 'CheckDevice is read-only' }
if (($PrepareRecovery -or $ResumeSystemReadback) -and -not $Execute) { throw 'Recovery preparation/resume requires explicit Execute' }
if (-not $AuthenticateOnly -and -not $Serial) { throw 'Specify the exact target Serial shown by adb devices -l' }
if ($Execute -and (-not $ConfirmInstall -or $ConfirmSerial -cne $Serial)) { throw 'Execution requires ConfirmInstall and ConfirmSerial matching Serial exactly' }
$adb = $null
# Distribution RSA/SHA256 verification uses authenticated in-memory bytes on macOS.
$bundlePath = (Get-Item -LiteralPath $Bundle -ErrorAction Stop).FullName
$keyPath = (Get-Item -LiteralPath $TrustedPublicKey -ErrorAction Stop).FullName
if ($keyPath.StartsWith($bundlePath.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) {
    throw 'Trusted public key must come from external host policy, outside the bundle'
}
$logDir = Join-Path $PSScriptRoot ('../logs/recovery-install-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $logDir -ErrorAction Stop | Out-Null
$held = [Collections.Generic.List[IO.FileStream]]::new()
$authenticatedChunks = @{}
$stage = '/tmp/t380-install-' + [guid]::NewGuid().ToString('N')
$stageOwned = $false
$stagedHashHelperRemote = $null
$stagedHashHelperBytes = $null
$mutex = [Threading.Mutex]::new($false,'T380RecoveryInstaller')
$mutexOwned = $false
function Open-Held([string]$Path) {
    $item = Get-Item -LiteralPath $Path -ErrorAction Stop
    if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Require regular public input: $Path" }
    $stream = [IO.File]::Open($item.FullName,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    $held.Add($stream)
    return $stream
}
function Hash-Held([IO.Stream]$Stream) {
    $Stream.Position = 0
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [Convert]::ToHexString($sha.ComputeHash($Stream)).ToLowerInvariant() }
    finally { $sha.Dispose(); $Stream.Position = 0 }
}
function Authenticate-ImageChunks([IO.FileStream]$Image,[string]$ExpectedHash,[string]$Name) {
    # macOS does not enforce Windows FileShare.Read against native writers.
    # Bind full-image and per-chunk hashes in the SAME traversal. Every later
    # buffer must match this authenticated traversal before any upload/write.
    $Image.Position = 0
    $whole = [Security.Cryptography.IncrementalHash]::CreateHash([Security.Cryptography.HashAlgorithmName]::SHA256)
    $chunks = [Collections.Generic.List[string]]::new()
    $buffer = [byte[]]::new(33554432)
    try {
        while ($Image.Position -lt $Image.Length) {
            $wanted = [int][Math]::Min(33554432L,$Image.Length - $Image.Position)
            $Image.ReadExactly($buffer,0,$wanted)
            $whole.AppendData($buffer,0,$wanted)
            $sha = [Security.Cryptography.SHA256]::Create()
            try { $chunks.Add([Convert]::ToHexString($sha.ComputeHash($buffer,0,$wanted)).ToLowerInvariant()) }
            finally { $sha.Dispose() }
        }
        if ([Convert]::ToHexString($whole.GetHashAndReset()).ToLowerInvariant() -cne $ExpectedHash) { throw "Authenticated image hash mismatch: $Name" }
        $authenticatedChunks[$Name] = $chunks
    } finally { $whole.Dispose(); $Image.Position = 0 }
}
function Read-Small([IO.Stream]$Stream,[long]$Maximum) {
    if ($Stream.Length -le 0 -or $Stream.Length -gt $Maximum) { throw 'Public metadata size outside bound' }
    $bytes = [byte[]]::new([int]$Stream.Length)
    $Stream.Position = 0
    $Stream.ReadExactly($bytes,0,$bytes.Length)
    return ,$bytes
}
function Write-HeldSnapshot([string]$Path,[byte[]]$Bytes) {
    $stream = [IO.File]::Open($Path,[IO.FileMode]::CreateNew,[IO.FileAccess]::ReadWrite,[IO.FileShare]::Read)
    $held.Add($stream)
    $stream.Write($Bytes,0,$Bytes.Length)
    $stream.Flush($true)
    $stream.Position = 0
}
function Reject-DuplicateJson([System.Text.Json.JsonElement]$Element) {
    if ($Element.ValueKind -eq [System.Text.Json.JsonValueKind]::Object) {
        $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
        foreach ($property in $Element.EnumerateObject()) {
            if (-not $names.Add($property.Name)) { throw 'Duplicate manifest JSON field' }
            Reject-DuplicateJson $property.Value
        }
    } elseif ($Element.ValueKind -eq [System.Text.Json.JsonValueKind]::Array) {
        foreach ($value in $Element.EnumerateArray()) { Reject-DuplicateJson $value }
    }
}
function Tablet([string]$Command) {
    $result = & $adb -s $serial shell -T $Command 2>&1
    $code = $LASTEXITCODE
    $text = ($result -join "`n").Trim()
    "UTC=$([DateTime]::UtcNow.ToString('o')) COMMAND=$Command EXIT=$code`n$text" | Add-Content (Join-Path $logDir 'commands.txt')
    if ($code -ne 0) { throw 'Recovery command failed; inspect receipt. No automatic replay or reboot.' }
    return $text
}
function Assert-DeviceIdentity {
    if ((Tablet 'getprop ro.product.device') -cne 'gta2swifi' -or
        (Tablet 'getprop ro.product.model') -cne 'SM-T380' -or
        (Tablet 'getprop ro.product.manufacturer') -cne 'samsung' -or
        (Tablet 'getprop ro.board.platform') -cne 'msm8937') {
        throw 'Unsupported device: require Samsung SM-T380 / gta2swifi / msm8937'
    }
    if ((Tablet 'getprop ro.twrp.version') -cne '3.7.0_9-0' -or
        (Tablet 'getprop ro.build.fingerprint') -cne 'Galaxy/omni_gta2swifi/gta2swifi:16.1.0/NJH47F/l05291255:eng/test-keys' -or
        (Tablet '/sbin/toybox id -u') -cne '0') { throw 'Require the supplied root rescue recovery' }
}
function Confirm-RecoveryImage {
    $node = Tablet 'readlink -f /dev/block/bootdevice/by-name/recovery'
    if ($node -cnotmatch '^/dev/block/(mmcblk0p[0-9]+)$') { throw 'Invalid recovery partition alias' }
    $name = $Matches[1]
    if ([long](Tablet "cat /sys/class/block/$name/size") * 512L -ne 33554432L) { throw 'Unexpected recovery partition size' }
    $actual = Hash-Remote (Readback-Command $name 33554432L) 33554432L 'recovery-identity'
    if ($actual -cne 'b1fa28acd454d145e0f650b216c36e3f361cd91988e97f46f65ea4fe923147fb') { throw 'Recovery differs from the exact supported rescue image' }
}
function Prepare-Recovery {
    # Device/partition/recovery identity and every signed host byte were checked first.
    # Abort on unexpected mounts rather than guessing how to unmount them.
    $targets = @()
    foreach ($line in (Tablet 'cat /proc/mounts') -split "`n") {
        $fields = $line -split ' '
        if ($fields[0] -match 'mmcblk0|/by-name/|/dev/block/dm-|/dev/mapper/') {
            if ($fields[1] -cnotin @('/data','/system','/system_root','/cache','/efs','/metadata','/vendor','/product','/system_ext','/odm')) { throw 'Unexpected block mount; prepare recovery manually' }
            $targets += $fields[1]
        }
    }
    Tablet 'echo 1 > /sys/class/power_supply/battery/batt_slate_mode && echo 0 > /sys/class/power_supply/sm5703-charger/charging_enabled && stop recovery' | Out-Null
    Assert-RecoverySafe
    foreach ($mount in ($targets | Sort-Object -Unique | Sort-Object { $_.Length } -Descending)) { Tablet "/sbin/toybox umount $mount" | Out-Null }
    Assert-Unmounted
}
function Assert-RecoverySafe {
    if ((Tablet 'cat /sys/class/power_supply/battery/batt_slate_mode') -cne '1' -or
        (Tablet 'cat /sys/class/power_supply/sm5703-charger/charging_enabled') -cne '0' -or
        (Tablet 'getprop init.svc.recovery') -cne 'stopped') {
        throw 'Require verified inhibited charging and stopped recovery service'
    }
}
function Assert-Unmounted {
    Assert-RecoverySafe
    $mounts = Tablet 'cat /proc/mounts'
    # Reject any eMMC/dm/mapper/by-name filesystem, including aliases, rather than
    # guessing which mounted filesystem may reach a write target.
    if ($mounts -match 'mmcblk0|/by-name/|/dev/block/dm-|/dev/mapper/') { throw 'Block filesystem mounted; coordinator must unmount before installation' }
    if ($mounts -notmatch '(?m)^\S+ /tmp tmpfs rw[, ]') { throw 'Require writable RAM tmpfs at /tmp' }
}
function Hash-Remote([string]$Command,[long]$ExpectedBytes,[string]$Name,[switch]$RetryStagedDaemonFailure) {
    # Never pass binary exec-out through PowerShell text/native pipelines.
    for ($attempt = 1; $attempt -le 2; $attempt++) {
        $psi = [Diagnostics.ProcessStartInfo]::new($adb)
        $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
        $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
        foreach ($arg in @('-s',$serial,'exec-out',$Command)) { $psi.ArgumentList.Add($arg) }
        $process = [Diagnostics.Process]::Start($psi)
        $errorTask = $process.StandardError.ReadToEndAsync()
        $sha = [Security.Cryptography.IncrementalHash]::CreateHash([Security.Cryptography.HashAlgorithmName]::SHA256)
        $buffer = [byte[]]::new(1048576)
        $count = 0L
        try {
            while (($read = $process.StandardOutput.BaseStream.Read($buffer,0,$buffer.Length)) -gt 0) {
                $count += [long]$read
                if ($count -gt $ExpectedBytes) { throw 'Readback exceeds exact authenticated length' }
                $sha.AppendData($buffer,0,$read)
            }
            $process.WaitForExit()
            $stderr = $errorTask.GetAwaiter().GetResult()
            "ATTEMPT=$attempt COMMAND=$Command`nEXIT=$($process.ExitCode) BYTES=$count EXPECTED=$ExpectedBytes`n$stderr" | Add-Content (Join-Path $logDir "$Name-readback.txt")
            if ($process.ExitCode -eq 0 -and $count -eq $ExpectedBytes) {
                return [Convert]::ToHexString($sha.GetHashAndReset()).ToLowerInvariant()
            }
            # Only a zero-byte, local Windows ADB daemon timeout can repeat the
            # read-only RAM-stage check. A partial stream or any other failure stops.
            $localDaemonTimeout = $process.ExitCode -eq -1 -and $count -eq 0 -and
                $stderr -match '(?s)\* daemon still not running.*error: cannot connect to daemon at tcp:5037: cannot connect to 127\.0\.0\.1:5037:.*\(10060\)'
            if (-not ($RetryStagedDaemonFailure -and $attempt -eq 1 -and $Command -ceq "cat $stage/chunk" -and $localDaemonTimeout)) {
                throw 'Readback failed or wrong exact length; do not reboot'
            }
            'Read-only RAM-stage verification retry after identified local ADB daemon timeout; no push or partition write repeated.' |
                Add-Content (Join-Path $logDir "$Name-readback.txt")
        } finally {
            $sha.Dispose()
            if (-not $process.HasExited) { $process.Kill($true); $process.WaitForExit() }
            # Retain counts and diagnostics even if an overlong/partial stream threw.
            $stderrReceipt = ''
            try { $stderrReceipt = $errorTask.GetAwaiter().GetResult() } catch { $stderrReceipt = $_.Exception.Message }
            @{attempt=$attempt;command=$Command;expectedBytes=$ExpectedBytes;rawCount=$count.ToString([Globalization.CultureInfo]::InvariantCulture);normalizedBytes=$count;exitCode=$process.ExitCode;stderr=$stderrReceipt} | ConvertTo-Json -Compress | Add-Content (Join-Path $logDir "$Name-readback-receipts.jsonl")
            $process.Dispose()
        }
    }
}
function Confirm-StagedChunk([string]$Remote,[long]$Count,[string]$ExpectedHash) {
    if ($Remote -cne "$stage/chunk" -or -not $stagedHashHelperRemote) { throw 'Unexpected staged hash target' }
    $length = Tablet "/sbin/toybox stat -c '%s' $Remote"
    if ($length -cne $Count.ToString([Globalization.CultureInfo]::InvariantCulture)) { throw 'RAM-staged chunk length mismatch' }
    $output = Tablet "$stagedHashHelperRemote $Remote"
    if ($output -cne "$ExpectedHash  $Remote") { throw 'RAM-staged chunk digest/path/format mismatch' }
}
function Install-StagedHashHelper([byte[]]$Bytes,[string]$ExpectedHash) {
    if ($stagedHashHelperRemote) { throw 'Staged hash helper already installed' }
    # Bootstrap trust with the original complete host readback. The unverified
    # executable is never used to authenticate itself or any image chunk.
    Push-HeldChunk $Bytes $Bytes.Length "$stage/sha256sum" 'sha256-helper'
    Tablet "/sbin/toybox chmod 0500 $stage/sha256sum" | Out-Null
    $script:stagedHashHelperRemote = "$stage/sha256sum"
    @{bytes=$Bytes.Length;sha256=$ExpectedHash;verification='complete-host-readback';remote=$stagedHashHelperRemote} |
        ConvertTo-Json | Set-Content (Join-Path $logDir 'sha256-helper-verified.json')
}
function Push-HeldChunk([byte[]]$Bytes,[int]$Count,[string]$Remote,[string]$Name) {
    $path = Join-Path $logDir 'transfer-chunk.bin'
    $chunk = [IO.File]::Open($path,[IO.FileMode]::Create,[IO.FileAccess]::ReadWrite,[IO.FileShare]::Read)
    try {
        $chunk.Write($Bytes,0,$Count); $chunk.Flush($true); $chunk.Position = 0
        $expected = Hash-Held $chunk
        & $adb -s $serial push $path $Remote 2>&1 | Add-Content (Join-Path $logDir "$Name-transfer.txt")
        if ($LASTEXITCODE -ne 0) { throw 'Chunk push failed; do not replay any ambiguous write' }
        if ($stagedHashHelperRemote -and $Remote -ceq "$stage/chunk") {
            Confirm-StagedChunk $Remote $Count $expected
        } else {
            $actual = Hash-Remote "cat $Remote" $Count "$Name-staged" -RetryStagedDaemonFailure
            if ($actual -cne $expected) { throw 'RAM-staged chunk differs from authenticated input' }
        }
    } finally { $chunk.Dispose() }
}
function Readback-Command([string]$Node,[long]$Length) {
    # Recovery exec-out merges remote stderr into the binary stream. Suppress
    # dd's records/bytes diagnostics on each command, retaining && exit ordering.
    if ($Length -gt 0 -and $Length % 1048576L -eq 0) {
        return "/sbin/toybox dd if=/dev/block/$Node bs=1048576 count=$([long]($Length / 1048576L)) 2>/dev/null"
    }
    $blocks = [long][Math]::Floor($Length / 4096)
    $offset = $blocks * 4096L
    $tail = $Length - $offset
    $commands = [Collections.Generic.List[string]]::new()
    if ($blocks -gt 0) { $commands.Add("/sbin/toybox dd if=/dev/block/$Node bs=4096 count=$blocks 2>/dev/null") }
    if ($tail -gt 0) { $commands.Add("/sbin/toybox dd if=/dev/block/$Node bs=1 skip=$offset count=$tail 2>/dev/null") }
    return $commands -join ' && '
}
function Final-DeviceHashCommand([ValidateSet('mmcblk0p45','mmcblk0p23')][string]$Node,[long]$Length) {
    if ($Length -le 0 -or -not $stagedHashHelperRemote) { throw 'Device hash requires exact positive length and verified helper' }
    $read = (Readback-Command $Node $Length).Replace(' 2>/dev/null','')
    # pipefail reports either the reader, tee or helper failing; wc proves exact
    # stream length even when dd reaches an early EOF without an error exit.
    # The FIFO counts the SAME stream the helper hashes, without a large RAM file.
    $command = @'
set -o pipefail || exit 90
p=__PREFIX__
rm -f "$p.fifo" "$p.count" "$p.digest" "$p.read-error" "$p.hash-error" "$p.tee-error" "$p.count-error"
/sbin/toybox mkfifo "$p.fifo" || exit 91
/sbin/toybox wc -c < "$p.fifo" > "$p.count" 2> "$p.count-error" &
count_pid=$!
{ __READ__; } 2> "$p.read-error" | /sbin/toybox tee "$p.fifo" 2> "$p.tee-error" | __HELPER__ > "$p.digest" 2> "$p.hash-error"
pipeline_rc=$?
wait "$count_pid"
count_rc=$?
rm -f "$p.fifo"
printf 'FORMAT=t380-final-sha256-v1\nPIPELINE_EXIT=%s\nCOUNT_EXIT=%s\nBYTES=' "$pipeline_rc" "$count_rc"
cat "$p.count"
printf 'DIGEST='
cat "$p.digest"
printf 'READ_DIAGNOSTICS_BEGIN\n'
cat "$p.read-error"
printf 'HASH_DIAGNOSTICS_BEGIN\n'
cat "$p.hash-error"
printf 'TEE_DIAGNOSTICS_BEGIN\n'
cat "$p.tee-error"
printf 'COUNT_DIAGNOSTICS_BEGIN\n'
cat "$p.count-error"
printf 'DIAGNOSTICS_END\n'
'@
    return $command.Replace('__PREFIX__',"$stage/final-$Node").Replace('__READ__',$read).Replace('__HELPER__',$stagedHashHelperRemote)
}
function Confirm-FinalImage([ValidateSet('mmcblk0p45','mmcblk0p23')][string]$Node,[long]$Length,[string]$Name,[string]$ExpectedHash) {
    $record = @{schema=1;node="/dev/block/$Node";offset=0;bytes=$Length;expectedSha256=$ExpectedHash;deviceAttempted=[bool]$stagedHashHelperRemote;verified=$false}
    $path = Join-Path $logDir "$Name-final-verification.json"
    $actual = $null
    try {
        if ($stagedHashHelperRemote) {
            try {
                $record.deviceOutput = Tablet (Final-DeviceHashCommand $Node $Length)
                $pattern = '\AFORMAT=t380-final-sha256-v1\r?\nPIPELINE_EXIT=([0-9]+)\r?\nCOUNT_EXIT=([0-9]+)\r?\nBYTES=[ \t]*(-?[0-9]+)[ \t]*\r?\nDIGEST=([0-9a-f]{64})  -\r?\nREAD_DIAGNOSTICS_BEGIN\r?\n'
                if ($record.deviceOutput -cnotmatch $pattern) { throw 'Malformed device hash/count receipt' }
                $record.pipelineExit=[int]$Matches[1]; $record.countExit=[int]$Matches[2]; $record.deviceCountRaw=$Matches[3]; $record.deviceSha256=$Matches[4]
                if ($record.pipelineExit -ne 0 -or $record.countExit -ne 0) { throw 'Device reader/count/hash pipeline failed' }
                # ARM32 recovery toybox wc prints its 32-bit counter as signed.
                # Fixed permitted image ranges are below 2^32: preserve raw text
                # and normalize only a valid signed32 value, then require the
                # exact authenticated length. Wide positive counters also work.
                if ($Length -le 0 -or $Length -ge 4294967296L) { throw 'Unsupported device counter range' }
                $record.deviceBytes=[long]$record.deviceCountRaw
                $record.deviceCountFormat='nonnegative'
                if ($record.deviceCountRaw.StartsWith('-')) {
                    if ($record.deviceBytes -lt -2147483648L -or $record.deviceBytes -ge 0) { throw 'Invalid signed32 device byte count' }
                    $record.deviceBytes+=4294967296L
                    $record.deviceCountFormat='signed32-normalized-to-uint32'
                } elseif ($record.deviceBytes -ge 4294967296L) { throw 'Device byte count exceeds supported range' }
                if ($record.deviceBytes -ne $Length) { throw 'Device hash stream differs from exact signed image length' }
                $actual=$record.deviceSha256; $record.method='device-sha256'
            } catch {
                $record.deviceFailure=$_.Exception.Message
                $record.method='full-host-readback-fallback'
            }
        } else {
            $record.deviceFailure='Verified device SHA256 helper unavailable'
            $record.method='full-host-readback-fallback'
        }
        # A successfully read exact range with a mismatching digest is an image
        # mismatch, not an availability failure. Never hide it with a retry.
        if ($actual -and $actual -cne $ExpectedHash) { throw "$Name device SHA256 mismatch; do not reboot" }
        if (-not $actual) { $actual=Hash-Remote (Readback-Command $Node $Length) $Length $Name }
        $record.sha256=$actual
        if ($actual -cne $ExpectedHash) { throw "$Name full SHA256 readback mismatch; do not reboot" }
        $record.verified=$true; $record.verifiedUtc=[DateTime]::UtcNow.ToString('o')
        @{bytes=$Length;sha256=$actual;verification=$record.method;verificationReceipt="$Name-final-verification.json";verifiedUtc=$record.verifiedUtc} | ConvertTo-Json | Set-Content (Join-Path $logDir "$Name-verified.json")
    } catch {
        $record.failure=$_.Exception.Message
        throw
    } finally {
        $record | ConvertTo-Json -Depth 5 | Set-Content $path
    }
}
function Confirm-ExistingSystem([IO.FileStream]$Image,[string]$ExpectedHash) {
    if ($Image.Length -ne 3674210304L) { throw 'Resume requires exact authenticated SYSTEM length' }
    Assert-Unmounted
    $actual = Hash-Remote (Readback-Command 'mmcblk0p45' $Image.Length) $Image.Length 'system-resume'
    if ($actual -cne $ExpectedHash) { throw 'Existing SYSTEM SHA256 mismatch; resume stopped before device writes' }
    @{bytes=$Image.Length;sha256=$actual;resumeReadback=$true;verifiedUtc=[DateTime]::UtcNow.ToString('o')} |
        ConvertTo-Json | Set-Content (Join-Path $logDir 'system-verified.json')
}
function Write-Image([IO.FileStream]$Image,[ValidateSet('mmcblk0p45','mmcblk0p23')][string]$Node,[string]$Name,[string]$ExpectedHash) {
    Assert-Unmounted
    $Image.Position = 0
    $buffer = [byte[]]::new(33554432)
    $offset = 0L
    while ($offset -lt $Image.Length) {
        $wanted = [int][Math]::Min(33554432L,$Image.Length - $offset)
        $Image.ReadExactly($buffer,0,$wanted)
        $chunkIndex = [int]($offset / 33554432L)
        $sha = [Security.Cryptography.SHA256]::Create()
        try { $chunkHash = [Convert]::ToHexString($sha.ComputeHash($buffer,0,$wanted)).ToLowerInvariant() }
        finally { $sha.Dispose() }
        if (-not $authenticatedChunks.ContainsKey($Name) -or $chunkIndex -ge $authenticatedChunks[$Name].Count -or $chunkHash -cne $authenticatedChunks[$Name][$chunkIndex]) { throw 'Image changed after authentication; stopped before this chunk upload/write. Do not reboot or replay.' }
        Push-HeldChunk $buffer $wanted "$stage/chunk" $Name
        if ($offset % 1048576L -ne 0) { throw 'Unaligned internal image offset' }
        $seek = [long]($offset / 1048576L)
        # Explicit toybox uses lseek64; standalone recovery dd is unsuitable.
        Tablet "/sbin/toybox dd if=$stage/chunk of=/dev/block/$Node bs=1048576 seek=$seek conv=notrunc && sync" | Out-Null
        $offset += [long]$wanted
        Write-Output "$Name written $offset / $($Image.Length) bytes"
    }
    Confirm-FinalImage $Node $Image.Length $Name $ExpectedHash
}
try {
    try { $mutexOwned = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $mutexOwned = $true }
    if (-not $mutexOwned) { throw 'Another private recovery installer is running' }
    $key = Open-Held $keyPath
    $keyBytes = Read-Small $key 65536
    if ([Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($keyBytes)).ToLowerInvariant() -cne $TrustedPublicKeySha256.ToLowerInvariant()) { throw 'External public key differs from trusted host pin' }
    $manifestStream = Open-Held (Join-Path $bundlePath 'manifest.json')
    $signatureStream = Open-Held (Join-Path $bundlePath 'manifest.sig')
    $receiptStream = Open-Held (Join-Path $bundlePath 'source-receipt.json')
    $system = Open-Held (Join-Path $bundlePath 'system.img')
    $boot = Open-Held (Join-Path $bundlePath 'boot.img')
    $manifestBytes = Read-Small $manifestStream 1048576
    $signatureBytes = Read-Small $signatureStream 16384
    $rsa = [Security.Cryptography.RSA]::Create()
    try {
        $rsa.ImportFromPem([Text.Encoding]::UTF8.GetString($keyBytes))
        if ($rsa.KeySize -ne 4096 -or -not $rsa.VerifyData($manifestBytes,$signatureBytes,[Security.Cryptography.HashAlgorithmName]::SHA256,[Security.Cryptography.RSASignaturePadding]::Pkcs1)) { throw 'Detached distribution signature verification failed' }
        'RSA4096 PKCS1 SHA256 signature verified against pinned in-memory public key and manifest bytes' | Set-Content (Join-Path $logDir 'authentication.txt')
    } finally { $rsa.Dispose() }
    $json = [System.Text.Json.JsonDocument]::Parse([ReadOnlyMemory[byte]]::new($manifestBytes))
    try {
        Reject-DuplicateJson $json.RootElement
        $manifest = [Text.Encoding]::UTF8.GetString($manifestBytes) | ConvertFrom-Json -AsHashtable
        $sequence = 0L
        if (-not $json.RootElement.GetProperty('sequence').TryGetInt64([ref]$sequence) -or $sequence -le $LastAcceptedSequence) { throw 'Sequence is not newer than external trusted host state' }
        if ($sequence -ne $ExpectedIssuedSequence) { throw 'Manifest differs from exact externally issued sequence' }
        $schema = 0L
        if (-not $json.RootElement.GetProperty('schema').TryGetInt64([ref]$schema) -or $schema -ne 1 -or $manifest.device -cne 'gta2swifi') { throw 'Unexpected manifest schema/device' }
        if ($manifest.images -isnot [Collections.IDictionary] -or $manifest.images.Count -ne 2 -or -not $manifest.images.Contains('boot.img') -or -not $manifest.images.Contains('system.img')) { throw 'Only BOOT and SYSTEM are permitted' }
        if ($manifest.source_receipt_sha256 -cnotmatch '^[0-9a-f]{64}$' -or (Hash-Held $receiptStream) -cne $manifest.source_receipt_sha256) { throw 'Source/build receipt hash mismatch' }
        foreach ($entry in @(@('system.img',$system,3674210304L,$true),@('boot.img',$boot,33554432L,$false))) {
            $name = $entry[0]; $image = $entry[1]; $size = 0L
            if (-not $json.RootElement.GetProperty('images').GetProperty($name).GetProperty('bytes').TryGetInt64([ref]$size) -or $size -le 0 -or $size -gt $entry[2] -or ($entry[3] -and $size -ne $entry[2]) -or $size -ne $image.Length) { throw "Invalid raw image length: $name" }
            $hash = $manifest.images[$name].sha256
            if ($hash -cnotmatch '^[0-9a-f]{64}$') { throw "Invalid image hash: $name" }
            Authenticate-ImageChunks $image $hash ($name -replace '\.img$','')
        }
    } finally { $json.Dispose() }
    $system.Position = 1080
    if ($system.ReadByte() -ne 0x53 -or $system.ReadByte() -ne 0xef) { throw 'SYSTEM must be raw ext4' }
    $bootMagic = [byte[]]::new(8); $boot.Position = 0
    $boot.ReadExactly($bootMagic,0,8)
    if ([Text.Encoding]::ASCII.GetString($bootMagic) -cne 'ANDROID!') { throw 'Invalid BOOT magic' }
    if ($AuthenticateOnly) {
        @{schema=1;status='AUTHENTICATED_OFFLINE';sequence=$sequence;lastAcceptedSequence=$LastAcceptedSequence;expectedIssuedSequence=$ExpectedIssuedSequence;manifestSha256=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($manifestBytes)).ToLowerInvariant();images=$manifest.images;deviceOperations=$false;hostSequenceAdvanced=$false} | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $logDir 'receipt.json')
        Write-Output "Authenticated fixed images offline. No device operation. Receipt: $logDir"
        return
    }
    # ADB is intentionally resolved only after offline authentication has returned.
    if ($AdbPath) { $adb = (Get-Item -LiteralPath $AdbPath -ErrorAction Stop).FullName }
    else { $adb = (Get-Command adb -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source }
    if (-not (Test-Path -LiteralPath $adb -PathType Leaf)) { throw 'ADB executable missing' }
    if ($StagedHashHelper -or $StagedHashHelperSha256) {
        if (-not $StagedHashHelper -or $StagedHashHelperSha256 -cnotmatch '^[0-9a-fA-F]{64}$') { throw 'Require helper path and explicit SHA256 pin together' }
        $helper = Open-Held $StagedHashHelper
        $stagedHashHelperBytes = Read-Small $helper 1048576
        $helperPin = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($stagedHashHelperBytes)).ToLowerInvariant()
        if ($helperPin -cne $StagedHashHelperSha256.ToLowerInvariant()) { throw 'Local staged hash helper differs from pin' }
    }
    Assert-DeviceIdentity
    foreach ($target in @(@('system','mmcblk0p45',3674210304L),@('boot','mmcblk0p23',33554432L),@('userdata','mmcblk0p48',11358154752L))) {
        foreach ($base in @('/dev/block/bootdevice/by-name/','/dev/block/platform/soc/7824900.sdhci/by-name/')) {
            if ((Tablet "readlink -f $base$($target[0])") -cne "/dev/block/$($target[1])") { throw 'Fixed partition alias mismatch' }
        }
        if ([long](Tablet "cat /sys/class/block/$($target[1])/size") * 512L -ne $target[2]) { throw 'Fixed partition capacity mismatch' }
        Tablet "test -b /dev/block/$($target[1])" | Out-Null
    }
    Confirm-RecoveryImage
    if ($CheckDevice) {
        @{status='DEVICE_COMPATIBILITY_CHECKED';serial=$Serial;sequence=$sequence;deviceWrites=$false;installationReadinessChecked=$false} | ConvertTo-Json | Set-Content (Join-Path $logDir 'receipt.json')
        Write-Output "Device and recovery identity verified. No device changes. Receipt: $logDir"
        return
    }
    if ($PrepareRecovery) { Prepare-Recovery }
    Assert-Unmounted
    Tablet '/sbin/toybox dd --help >/dev/null' | Out-Null
    $receipt = @{schema=1;serial=$Serial;sequence=$sequence;lastAcceptedSequence=$LastAcceptedSequence;expectedIssuedSequence=$ExpectedIssuedSequence;trustedPublicKeySha256=$TrustedPublicKeySha256;images=$manifest.images;execute=[bool]$Execute;prepareRecovery=[bool]$PrepareRecovery;resumeSystemReadback=[bool]$ResumeSystemReadback;runtimeAccepted=$false;hostSequenceAdvanced=$false}
    $receipt | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $logDir 'receipt.json')
    if (-not $Execute) { Write-Output "Authenticated preflight passed. No device writes. Receipt: $logDir"; return }
    # Explicit coordinator recovery from a completed SYSTEM write: prove the
    # whole existing partition before even creating a RAM stage, then skip only
    # SYSTEM rewriting. All authentication/identity/size/unmount gates above run.
    if ($ResumeSystemReadback) { Confirm-ExistingSystem $system $manifest.images['system.img'].sha256 }
    Tablet "test ! -e $stage && test ! -L $stage && mkdir -m 0700 $stage" | Out-Null
    $stageOwned = $true
    if ($stagedHashHelperBytes) { Install-StagedHashHelper $stagedHashHelperBytes $helperPin }
    if (-not $ResumeSystemReadback) { Write-Image $system 'mmcblk0p45' 'system' $manifest.images['system.img'].sha256 }
    # BOOT is last and unreachable if SYSTEM verification or unmount checks failed.
    Write-Image $boot 'mmcblk0p23' 'boot' $manifest.images['boot.img'].sha256
    Write-Output "Authenticated images installed and exact-range SHA256 verified. No reboot or host sequence advance. Runtime acceptance remains coordinator-owned. Receipt: $logDir"
} catch {
    @{schema=1;status='FAILED';serial=$Serial;sequence=36;execute=[bool]$Execute;message=$_.Exception.Message;utc=[DateTime]::UtcNow.ToString('o');automaticReboot=$false;automaticRetry=$false} | ConvertTo-Json | Set-Content (Join-Path $logDir 'failure.json')
    throw
} finally {
    # Cleanup never replays a partition write and only removes this process's RAM stage.
    if ($stageOwned) {
        try { Tablet "rm -f $stage/chunk $stage/sha256sum $stage/final-mmcblk0p45.* $stage/final-mmcblk0p23.* && rmdir $stage" | Out-Null }
        catch { Write-Warning "RAM stage cleanup failed; inspect $logDir" }
    }
    foreach ($stream in $held) { $stream.Dispose() }
    $chunkPath = Join-Path $logDir 'transfer-chunk.bin'
    if (Test-Path -LiteralPath $chunkPath) {
        try { Remove-Item -LiteralPath $chunkPath }
        catch { Write-Warning "Host chunk cleanup failed; inspect $logDir" }
    }
    if ($mutexOwned) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}
