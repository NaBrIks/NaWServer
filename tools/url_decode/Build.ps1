param([string]$PlatformIO=(Join-Path $env:USERPROFILE '.platformio/penv/Scripts/pio.exe'))
$ErrorActionPreference='Stop'
$repo=Resolve-Path (Join-Path $PSScriptRoot '../..')
if(!$env:PLATFORMIO_CORE_DIR){$env:PLATFORMIO_CORE_DIR=Join-Path $env:USERPROFILE '.platformio'}
New-Item -ItemType Directory -Force (Join-Path $repo 'local') | Out-Null
& $PlatformIO run -d (Join-Path $PSScriptRoot 'device') -e c3 -j 2
if($LASTEXITCODE){throw 'C3 build failed'}
Get-FileHash (Join-Path $PSScriptRoot 'device/.pio/build/c3/firmware.bin') -Algorithm SHA256
