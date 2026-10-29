# SPDX-License-Identifier: Apache-2.0
$ErrorActionPreference='Stop'
$path=Join-Path $PSScriptRoot '../install.ps1'
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($path,[ref]$tokens,[ref]$errors)
if($errors.Count){throw ($errors|Out-String)}
foreach($f in $ast.FindAll({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst]},$true)){Invoke-Expression $f.Extent.Text}
$script:calls=@();$script:properties=@{}
function Tablet([string]$Command){
    $script:calls+=,$Command
    if($script:properties.ContainsKey($Command)){return $script:properties[$Command]}
    throw "Unexpected device command in test: $Command"
}
function ExpectFailure([scriptblock]$Action,[string]$Message,[string]$ExpectedSubstring=''){
    $failed=$false;try{& $Action}catch{$failed=$true;if($ExpectedSubstring -and -not $_.Exception.Message.Contains($ExpectedSubstring)){throw "Wrong rejection for ${Message}: $($_.Exception.Message)"}}
    if(-not $failed){throw "Expected rejection: $Message"}
}
function GoodProperties {
    $script:properties=@{'getprop ro.product.device'='gta2swifi';'getprop ro.product.model'='SM-T380';'getprop ro.product.manufacturer'='samsung';'getprop ro.board.platform'='msm8937';'getprop ro.twrp.version'='3.7.0_9-0';'getprop ro.build.fingerprint'='Galaxy/omni_gta2swifi/gta2swifi:16.1.0/NJH47F/l05291255:eng/test-keys';'/sbin/toybox id -u'='0'}
    $script:calls=@()
}
GoodProperties
Assert-DeviceIdentity
$cases=1
foreach($property in @('getprop ro.product.device','getprop ro.product.model','getprop ro.product.manufacturer','getprop ro.board.platform','getprop ro.twrp.version','getprop ro.build.fingerprint','/sbin/toybox id -u')){
    GoodProperties;$script:properties[$property]='wrong'
    ExpectFailure {Assert-DeviceIdentity} $property
    $cases++
}
GoodProperties
$script:properties['readlink -f /dev/block/bootdevice/by-name/recovery']='/dev/block/mmcblk0p24'
$script:properties['cat /sys/class/block/mmcblk0p24/size']='65536'
$script:recoveryHash='b1fa28acd454d145e0f650b216c36e3f361cd91988e97f46f65ea4fe923147fb'
function Hash-Remote([string]$Command,[long]$ExpectedBytes,[string]$Name){
    if($ExpectedBytes -ne 33554432 -or $Name -cne 'recovery-identity'){throw 'Wrong recovery read range'}
    return $script:recoveryHash
}
Confirm-RecoveryImage;$cases++
$script:recoveryHash='a'*64;ExpectFailure {Confirm-RecoveryImage} 'changed recovery';$cases++
$script:properties['readlink -f /dev/block/bootdevice/by-name/recovery']='/dev/block/mmcblk0p24;reboot'
ExpectFailure {Confirm-RecoveryImage} 'recovery alias injection';$cases++
GoodProperties
$script:properties['cat /proc/mounts']='/dev/block/mmcblk0p48 /unexpected ext4 rw 0 0'
ExpectFailure {Prepare-Recovery} 'unknown mount'
if($script:calls.Count -ne 1){throw 'Recovery modified before unknown mount was rejected'}
$cases++
$command=Readback-Command 'mmcblk0p23' 17326096
if($command -cne '/sbin/toybox dd if=/dev/block/mmcblk0p23 bs=4096 count=4230 2>/dev/null && /sbin/toybox dd if=/dev/block/mmcblk0p23 bs=1 skip=17326080 count=16 2>/dev/null'){throw 'BOOT exact tail changed'}
$cases++

# Mutating a previously authenticated file must fail before any upload or partition write.
$file=Join-Path ([IO.Path]::GetTempPath()) ('t380-mutation-'+[guid]::NewGuid().ToString('N'))
$stream=[IO.File]::Open($file,[IO.FileMode]::CreateNew,[IO.FileAccess]::ReadWrite,[IO.FileShare]::Read)
try {
    $data=[byte[]]::new(4096);$stream.Write($data);$stream.Flush()
    $hash=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($data)).ToLowerInvariant()
    $authenticatedChunks=@{}
    Authenticate-ImageChunks $stream $hash 'boot'
    $stream.Position=100;$stream.WriteByte(1);$stream.Flush()
    function Assert-Unmounted {}
    $script:pushCalls=0
    function Push-HeldChunk {$script:pushCalls++;throw 'Unexpected upload'}
    ExpectFailure {Write-Image $stream 'mmcblk0p23' 'boot' $hash} 'changed image after authentication'
    if($script:pushCalls){throw 'Changed image was uploaded'}
    $cases++
} finally {$stream.Dispose();Remove-Item -LiteralPath $file}

# Conflicting modes must fail before resolving even the nonexistent input bundle.
foreach($case in @(
    @{arguments=@{AuthenticateOnly=$true;Execute=$true};message='Offline authentication'},
    @{arguments=@{CheckDevice=$true;Execute=$true};message='CheckDevice is read-only'},
    @{arguments=@{PrepareRecovery=$true};message='requires explicit Execute'},
    @{arguments=@{Execute=$true;Serial='example';ConfirmSerial='different';ConfirmInstall=$true};message='ConfirmSerial matching Serial'}
)) {
    $arguments=$case.arguments
    ExpectFailure {& $path -Bundle 'absent-bundle' -LastAcceptedSequence 0 @arguments} 'unsafe argument combination' $case.message
    $cases++
}

# Exercise the real binary-stream reader with a local fake process, never ADB.
$realHash=$ast.FindAll({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Hash-Remote'},$true)[0]
Invoke-Expression $realHash.Extent.Text
$logDir=Join-Path ([IO.Path]::GetTempPath()) ('t380-stream-tests-'+[guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($logDir)|Out-Null
$serial=Join-Path $logDir 'fake_process.py'
$python=Get-Command python3 -CommandType Application -ErrorAction SilentlyContinue
if(-not $python){$python=Get-Command python -CommandType Application -ErrorAction Stop}
$adb=($python | Select-Object -First 1).Source
@'
import sys
mode=sys.argv[-1]
sys.stderr.write('retained diagnostic '+mode+'\n')
sys.stderr.flush()
sys.stdout.buffer.write({'exact':b'DATA','short':b'DAT','long':b'DATAX','failed':b'DAT'}[mode])
sys.stdout.buffer.flush()
sys.exit(7 if mode=='failed' else 0)
'@ | Set-Content -Encoding utf8 $serial
try {
    $expectedHash=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::ASCII.GetBytes('DATA'))).ToLowerInvariant()
    if((Hash-Remote 'exact' 4 'exact') -cne $expectedHash){throw 'Binary stream digest differs'}
    $cases++
    foreach($mode in @('short','long','failed')){
        ExpectFailure {Hash-Remote $mode 4 $mode} "readback $mode"
        $receipt=Get-Content (Join-Path $logDir "$mode-readback-receipts.jsonl") -Tail 1 | ConvertFrom-Json
        $expectedCount=if($mode -eq 'long'){5}else{3}
        if($receipt.normalizedBytes -ne $expectedCount -or $receipt.rawCount -cne "$expectedCount" -or $receipt.stderr -notmatch "retained diagnostic $mode"){throw 'Failed readback lost count/diagnostic receipt'}
        $cases++
    }
} finally {Remove-Item -LiteralPath $logDir -Recurse -Force}
@{status='PASS';caseCount=$cases;deviceActions=$false}|ConvertTo-Json
