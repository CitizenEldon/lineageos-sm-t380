$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
function Functions([string]$path){
 $tokens=$null;$errors=$null
 $ast=[Management.Automation.Language.Parser]::ParseFile($path,[ref]$tokens,[ref]$errors)
 if($errors.Count){throw ($errors|Out-String)}
 $r=@{};foreach($f in $ast.FindAll({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst]},$true)){$r[$f.Name]=$f.Extent.Text};return $r
}
$after=Functions (Join-Path $PSScriptRoot '../install.ps1')
foreach($name in @('Confirm-FinalImage','Final-DeviceHashCommand','Readback-Command')){Invoke-Expression $after[$name]}
$logDir=Join-Path ([IO.Path]::GetTempPath()) ('t380-counter-host-test-'+[guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($logDir)|Out-Null
$stage=$logDir;$stagedHashHelperRemote='/mock/helper'
$installed=Get-Content (Join-Path $PSScriptRoot 'fixtures/device-verification.json') -Raw|ConvertFrom-Json
$proof=$installed.installed.finalVerification.system
$expected=$proof.expectedSha256
function Tablet([string]$Command){return $script:deviceOutput}
function Hash-Remote([string]$Command,[long]$ExpectedBytes,[string]$Name){
 $script:hostCalls++;$script:hostBytes=$ExpectedBytes
 if($script:hostError){throw 'host read error'}
 return $script:hostHash
}
$cases=@()
function Case([string]$Name,[long]$Length,[string]$Count,[string]$Want,[string]$Digest=$expected,[int]$PipelineExit=0,[int]$CountExit=0,[string]$HostMode='good'){
 $script:hostCalls=0;$script:hostError=$HostMode -eq 'error';$script:hostHash=if($HostMode -eq 'mismatch'){'a'*64}else{$expected}
 $script:deviceOutput="FORMAT=t380-final-sha256-v1`nPIPELINE_EXIT=$PipelineExit`nCOUNT_EXIT=$CountExit`nBYTES=$Count`nDIGEST=$Digest  -`nREAD_DIAGNOSTICS_BEGIN`nread stats`nHASH_DIAGNOSTICS_BEGIN`nTEE_DIAGNOSTICS_BEGIN`nCOUNT_DIAGNOSTICS_BEGIN`nDIAGNOSTICS_END"
 if($Name -eq 'actual-system-receipt'){$script:deviceOutput=$script:actualReceipt}
 $failed=$false;try{Confirm-FinalImage 'mmcblk0p45' $Length $Name $expected}catch{$failed=$true}
 $r=Get-Content (Join-Path $logDir "$Name-final-verification.json") -Raw|ConvertFrom-Json
 switch($Want){
  'device'{if($failed -or -not $r.verified -or $script:hostCalls -or $r.method -cne 'device-sha256' -or $r.deviceBytes -ne $Length -or $r.deviceCountRaw -cne $Count){throw "$Name device verification incorrect"}
   $format=if($Count.StartsWith('-')){'signed32-normalized-to-uint32'}else{'nonnegative'}
   if($r.deviceCountFormat -cne $format){throw "$Name counter provenance lost"}}
  'fallback'{if($failed -or -not $r.verified -or $script:hostCalls -ne 1 -or $script:hostBytes -ne $Length -or $r.method -cne 'full-host-readback-fallback' -or -not $r.deviceFailure){throw "$Name fallback incorrect"}}
  'fatal-digest'{if(-not $failed -or $r.verified -or $script:hostCalls){throw "$Name mismatch was hidden by retry"}}
  'fatal-host'{if(-not $failed -or $r.verified -or $script:hostCalls -ne 1){throw "$Name host failure accepted"}}
 }
 $script:cases+=@{name=$Name;result='PASS';rawCount=$Count;length=$Length;expectedOutcome=$Want;hostReadbacks=$script:hostCalls;receipt=$r}
}
try{
 $script:actualReceipt=$proof.deviceOutput
 Case 'actual-system-receipt' 3674210304 '-620756992' 'device'
 Case 'signed32-minimum' 2147483648 '-2147483648' 'device'
 Case 'uint32-maximum' 4294967295 '-1' 'device'
 Case 'signed32-maximum' 2147483647 '2147483647' 'device'
 Case 'wide-positive-system' 3674210304 '3674210304' 'device'
 Case 'small-positive-boot' 17326096 '17326096' 'device'
 Case 'negative-zero' 1 '-0' 'fallback'
 Case 'below-signed32-minimum' 2147483647 '-2147483649' 'fallback'
 Case 'uint32-overflow' 1 '4294967296' 'fallback'
 Case 'int64-overflow-text' 1 '9223372036854775808' 'fallback'
 Case 'malformed-text' 1 'nan' 'fallback'
 Case 'unexpected-plus' 1 '+1' 'fallback'
 Case 'normalized-short' 3674210304 '-620756993' 'fallback'
 Case 'normalized-long' 3674210304 '-620756991' 'fallback'
 Case 'signed32-invalid-for-small' 1 '-1' 'fallback'
 Case 'requested-zero' 0 '0' 'fallback'
 Case 'requested-uint32-overflow' 4294967296 '0' 'fallback'
 Case 'pipeline-error' 3674210304 '-620756992' 'fallback' $expected 1
 Case 'counter-error' 3674210304 '-620756992' 'fallback' $expected 0 1
 Case 'normalized-digest-mismatch' 3674210304 '-620756992' 'fatal-digest' ('a'*64)
 Case 'positive-digest-mismatch' 17326096 '17326096' 'fatal-digest' ('a'*64)
 Case 'fallback-host-mismatch' 3674210304 '-620756993' 'fatal-host' $expected 0 0 'mismatch'
 Case 'fallback-host-error' 3674210304 '-620756993' 'fatal-host' $expected 0 0 'error'
 @{status='PASS';caseCount=$cases.Count;deviceActions=$false;actualSystemReceiptReplayed=$true}|ConvertTo-Json
}finally{Remove-Item -LiteralPath $logDir -Recurse -Force}
