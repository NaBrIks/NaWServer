param([string]$PlatformIO=(Join-Path $env:USERPROFILE '.platformio/penv/Scripts/pio.exe'),[switch]$Integration)
$ErrorActionPreference='Stop'
$repo=Resolve-Path (Join-Path $PSScriptRoot '../..')
if(!$env:PLATFORMIO_CORE_DIR){$env:PLATFORMIO_CORE_DIR=Join-Path $env:USERPROFILE '.platformio'}
$device=if($Integration){'integration_device'}else{'device'}
& $PlatformIO run -d (Join-Path $PSScriptRoot $device) -e c3 -j 2
if($LASTEXITCODE){throw 'SSE C3 build failed'}
Get-FileHash (Join-Path $PSScriptRoot "$device/.pio/build/c3/firmware.bin") -Algorithm SHA256
