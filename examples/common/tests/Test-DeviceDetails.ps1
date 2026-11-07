param([string]$ZigPath=(Join-Path $PSScriptRoot '../../../local/host-tools/zig-x86_64-windows-0.14.1/zig.exe'))
$ErrorActionPreference='Stop'
$repository=(Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$vendor=Join-Path $repository 'examples/showcase/managed_components/espressif__cbor/tinycbor/src'
$build=Join-Path $repository 'local/device-info-tests'
New-Item -ItemType Directory -Path $build -Force | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR=Join-Path $build 'zig-cache'
Push-Location $build
try {
 $objects=@()
 foreach($name in @('cborencoder','cborencoder_close_container_checked','cborencoder_float')) {
  & $ZigPath cc -std=c11 -O1 -I $vendor -c (Join-Path $vendor "$name.c") -o "$name.obj"
  if($LASTEXITCODE){throw "TinyCBOR build failed: $name"}
  $objects+=Join-Path $build "$name.obj"
 }
 foreach($target in @(@('c3','showcase','showcase'),@('s3','touch_lcd7','lcd7_web'),@('c6','c6_lcd147','c6_demo'))) {
  $targetName=$target[0];$project=$target[1]
  $defines=@('-DCONFIG_IDF_TARGET_ESP32C3=0','-DCONFIG_IDF_TARGET_ESP32S3=0','-DCONFIG_IDF_TARGET_ESP32C6=0') | Where-Object {$_ -notmatch "ESP32$($targetName.ToUpper())="}
  $defines+="-DCONFIG_IDF_TARGET_ESP32$($targetName.ToUpper())=1"
  & $ZigPath c++ -std=c++17 -O1 -Wall -Wextra @defines -I $vendor -I (Join-Path $PSScriptRoot 'stubs') (Join-Path $PSScriptRoot 'device_details_test.cpp') (Join-Path $repository "examples/$project/main/device_details.cpp") @objects -o "$targetName.exe"
  if($LASTEXITCODE){throw "Collector build failed: $targetName"}
  & (Join-Path $build "$targetName.exe") $build $targetName
  if($LASTEXITCODE){throw "Collector test failed: $targetName"}
 }
}finally{Pop-Location}
& node (Join-Path $PSScriptRoot 'verify-payloads.mjs') $build
if($LASTEXITCODE){throw 'Payload contract checks failed'}
