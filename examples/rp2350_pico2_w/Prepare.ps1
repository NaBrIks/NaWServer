[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
$dependencies=Join-Path $PSScriptRoot '.deps'
New-Item -ItemType Directory -Force -Path $dependencies | Out-Null
$kernel=Join-Path $dependencies 'FreeRTOS-Kernel'
if(-not (Test-Path "$kernel/tasks.c")) {
    & git clone --depth 1 --branch V11.3.3 https://github.com/FreeRTOS/FreeRTOS-Kernel.git $kernel
    if($LASTEXITCODE) { throw 'FreeRTOS download failed' }
}
& git -C $kernel submodule update --init --depth 1
if($LASTEXITCODE) { throw 'FreeRTOS port download failed' }
$littlefs=Join-Path $dependencies 'littlefs'
if(-not (Test-Path "$littlefs/lfs.c")) {
    & git clone --depth 1 --branch v2.11.3 https://github.com/littlefs-project/littlefs.git $littlefs
    if($LASTEXITCODE) { throw 'LittleFS download failed' }
}
Write-Host 'Demo-local dependencies are ready. Shared SDK/tools are reused, not changed.'
