[CmdletBinding()]
param([string]$SdkRoot = (Join-Path $env:USERPROFILE '.pico-sdk/sdk/2.3.1'))
$ErrorActionPreference='Stop'
if (-not (Test-Path "$PSScriptRoot/.private/wifi_config.h")) {
    throw 'Prepare the demo-local .private/wifi_config.h (see README.md)'
}
& node "$PSScriptRoot/tools/prepare-assets.mjs"
if($LASTEXITCODE) { throw 'Asset preparation failed' }
$tools=Join-Path $env:USERPROFILE '.pico-sdk'
& cmake -S $PSScriptRoot -B "$PSScriptRoot/build" -G Ninja `
    "-DPICO_SDK_PATH=$SdkRoot" "-DPICO_TOOLCHAIN_PATH=$tools/toolchain/14_2_Rel1" `
    "-Dpicotool_DIR=$tools/picotool/2.3.1/picotool" "-Dpioasm_DIR=$tools/tools/2.3.1/pioasm" `
    "-DCMAKE_MAKE_PROGRAM=$tools/ninja/1.13.1/ninja.exe" -DCMAKE_BUILD_TYPE=Release
if($LASTEXITCODE) { throw 'CMake failed' }
& cmake --build "$PSScriptRoot/build" --parallel 8
if($LASTEXITCODE) { throw 'Firmware build failed' }
