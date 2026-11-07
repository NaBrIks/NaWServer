[CmdletBinding()]
param(
 [ValidateSet('esp32','esp32c3','esp32c6','esp32s3')][string]$Target='esp32c3',
 [ValidateSet('sse','ws')][string]$StatusProtocol='sse',
 [ValidateSet('baseline','lowmem','tight','readme')][string]$NAWSERVER_DEMO_MEMORY_PROFILE='baseline',
 [ValidateRange(0,3)][int]$NAWSERVER_DEMO_README_PROFILE=0,
 [switch]$NAWSERVER_DEMO_ASYNC_PROBE,
 [ValidateRange(0,2)][int]$NAWSERVER_DEMO_TX_MEMORY_MODE=0,
 [ValidateRange(0,1)][int]$NAWSERVER_DEMO_TX_EXECUTOR=0,
 [ValidateRange(0,2)][int]$NAWSERVER_DEMO_TX_METADATA_MODE=0,
 [ValidateRange(256,65536)][int]$NAWSERVER_DEMO_TX_METADATA_STATIC_BYTES=2048,
 [ValidateRange(0,1)][int]$NAWSERVER_DEMO_TX_PAYLOAD_PSRAM=0,
 [ValidateRange(0,1)][int]$NAWSERVER_DEMO_TX_INTERNAL_ONLY=1,
 [ValidateRange(2,12)][int]$NAWSERVER_DEMO_TX_BLOCKS=4,
 [ValidateRange(2,8)][int]$NAWSERVER_DEMO_HTTP_JOBS=2,
 [ValidateRange(4,12)][int]$NAWSERVER_DEMO_CONNECTIONS=4,
 [ValidateRange(1,12)][int]$NAWSERVER_DEMO_INGRESS_SLOTS=2,
 [ValidateRange(768,4096)][int]$NAWSERVER_DEMO_TX_BLOCK_BYTES=1024,
 [ValidateSet('off','on')][string]$NAWSERVER_DEMO_PSRAM='off',
 [ValidateSet('caps','wide')][string]$NAWSERVER_DEMO_PSRAM_PROFILE='caps',
 [ValidateSet(6144,8192)][int]$NAWSERVER_DEMO_MAIN_STACK=8192,
 [switch]$NAWSERVER_DEMO_STACK_DIAGNOSTICS,
 [switch]$NAWSERVER_DEMO_MEMORY_PROBE,
 [switch]$NAWSERVER_DEMO_MEMORY_BREAKDOWN,
 [switch]$NAWSERVER_DEMO_MEMORY_TRACE,
 [ValidateRange(0,4)][int]$NAWSERVER_DEMO_MEMORY_LINK_PROFILE=0,
 [switch]$NAWSERVER_DEMO_MEMORY_SAMPLER,
 [switch]$NAWSERVER_DEMO_FULL_STOP_PROBE,
 [switch]$NAWSERVER_DEMO_TX_BULK_PROBE,
 [switch]$NAWSERVER_DEMO_NATIVE_ACCEPT_DIAGNOSTICS,
 [string]$Version='0.1.0-compact',
 [string]$WifiHeader='',
 [string]$SdkRoot=(Join-Path $env:USERPROFILE '.platformio/packages/framework-espidf-v5.5.6'),
 [string]$BuildDirectory=(Join-Path $PSScriptRoot '.build/esp32c3')
)
$ErrorActionPreference='Stop'
if($NAWSERVER_DEMO_ASYNC_PROBE -and $NAWSERVER_DEMO_README_PROFILE -ne 3){throw 'NAWSERVER_DEMO_ASYNC_PROBE requires NAWSERVER_DEMO_README_PROFILE=3'}
if($NAWSERVER_DEMO_MEMORY_BREAKDOWN -and $NAWSERVER_DEMO_MEMORY_PROBE){throw 'Select one startup measurement harness.'}
if($NAWSERVER_DEMO_MEMORY_LINK_PROFILE -and ($NAWSERVER_DEMO_MEMORY_BREAKDOWN -or $NAWSERVER_DEMO_MEMORY_PROBE -or $NAWSERVER_DEMO_MEMORY_SAMPLER -or $NAWSERVER_DEMO_MEMORY_TRACE)){
 throw 'Link-only profiles cannot run a hardware measurement harness.'
}
$traceConfigPath=Join-Path $BuildDirectory 'sdkconfig'
if(Test-Path -LiteralPath $traceConfigPath){
 $traceConfig=Get-Content -LiteralPath $traceConfigPath -Raw
 $traceWasEnabled=$traceConfig -match '(?m)^CONFIG_HEAP_TRACING_STANDALONE=y\r?$'
 if($traceWasEnabled -ne $NAWSERVER_DEMO_MEMORY_TRACE.IsPresent){throw 'Heap tracing changes code and memory. Use a fresh -BuildDirectory when switching it.'}
}
if(($NAWSERVER_DEMO_TX_BLOCK_BYTES % 16) -ne 0){throw 'NAWSERVER_DEMO_TX_BLOCK_BYTES must be a multiple of 16 for arena alignment.'}
if($NAWSERVER_DEMO_CONNECTIONS -lt ($NAWSERVER_DEMO_HTTP_JOBS+2)){throw 'NAWSERVER_DEMO_CONNECTIONS must accommodate HTTP jobs plus two SSE connections.'}
if($NAWSERVER_DEMO_PSRAM -eq 'on' -and $Target -ne 'esp32s3'){throw 'The PSRAM profile is defined only for the identified 8 MiB octal ESP32-S3.'}
if(($NAWSERVER_DEMO_TX_METADATA_MODE -eq 2 -or $NAWSERVER_DEMO_TX_PAYLOAD_PSRAM) -and ($Target -ne 'esp32s3' -or $NAWSERVER_DEMO_PSRAM -ne 'on')){throw 'PSRAM metadata/payload requires -Target esp32s3 -NAWSERVER_DEMO_PSRAM on.'}
if($NAWSERVER_DEMO_TX_PAYLOAD_PSRAM -and $NAWSERVER_DEMO_TX_MEMORY_MODE -eq 2){throw 'Static TX payload storage cannot be combined with PSRAM TX payload.'}
if($NAWSERVER_DEMO_PSRAM -eq 'off' -and $NAWSERVER_DEMO_PSRAM_PROFILE -ne 'caps'){throw 'A PSRAM heap profile requires -NAWSERVER_DEMO_PSRAM on.'}
if($NAWSERVER_DEMO_FULL_STOP_PROBE -and -not ($NAWSERVER_DEMO_MEMORY_PROBE -or $NAWSERVER_DEMO_MEMORY_SAMPLER)){throw 'The full Demo stop probe requires -NAWSERVER_DEMO_MEMORY_PROBE or -NAWSERVER_DEMO_MEMORY_SAMPLER.'}
if($NAWSERVER_DEMO_TX_BULK_PROBE -and ($Target -ne 'esp32s3' -or $StatusProtocol -ne 'sse')){throw 'The TX bulk probe is defined for the ESP32-S3 SSE diagnostic profile.'}
if($NAWSERVER_DEMO_NATIVE_ACCEPT_DIAGNOSTICS -and -not $NAWSERVER_DEMO_MEMORY_SAMPLER){throw 'Native accept diagnostics require -NAWSERVER_DEMO_MEMORY_SAMPLER for bounded UART sampling.'}
$privateDirectory=Join-Path $PSScriptRoot '.private'
$privateHeader=Join-Path $privateDirectory 'wifi_secret.hpp'
if($WifiHeader){
 if(-not (Test-Path -LiteralPath $WifiHeader -PathType Leaf)){throw 'The supplied -WifiHeader file does not exist'}
 New-Item -ItemType Directory -Force $privateDirectory | Out-Null
 $sourceHeader=(Resolve-Path -LiteralPath $WifiHeader).ProviderPath
 if(-not [string]::Equals($sourceHeader,[IO.Path]::GetFullPath($privateHeader),[StringComparison]::OrdinalIgnoreCase)){
  Copy-Item -LiteralPath $sourceHeader -Destination $privateHeader
 }
}
if(-not (Test-Path -LiteralPath $privateHeader -PathType Leaf)){throw 'Provide -WifiHeader once to prepare the compact demo Wi-Fi configuration'}
if(-not $PSBoundParameters.ContainsKey('BuildDirectory')){
 $suffix=if($NAWSERVER_DEMO_MEMORY_PROFILE -eq 'baseline'){''}else{"-$NAWSERVER_DEMO_MEMORY_PROFILE"}
 if($NAWSERVER_DEMO_TX_MEMORY_MODE -ne 0){$suffix+="-tx$NAWSERVER_DEMO_TX_MEMORY_MODE"}
 if($NAWSERVER_DEMO_TX_METADATA_MODE -ne 0){$suffix+="-metadata$NAWSERVER_DEMO_TX_METADATA_MODE"}
 if($NAWSERVER_DEMO_TX_METADATA_MODE -eq 1 -and $NAWSERVER_DEMO_TX_METADATA_STATIC_BYTES -ne 2048){$suffix+="-metabytes$NAWSERVER_DEMO_TX_METADATA_STATIC_BYTES"}
 if($NAWSERVER_DEMO_TX_PAYLOAD_PSRAM){$suffix+='-payloadpsram'}
 if(-not $NAWSERVER_DEMO_TX_INTERNAL_ONLY){$suffix+='-txsystem'}
 if($NAWSERVER_DEMO_TX_BLOCKS -ne 4){$suffix+="-blocks$NAWSERVER_DEMO_TX_BLOCKS"}
 if($NAWSERVER_DEMO_HTTP_JOBS -ne 2 -or $NAWSERVER_DEMO_CONNECTIONS -ne 4){$suffix+="-jobs$NAWSERVER_DEMO_HTTP_JOBS-conn$NAWSERVER_DEMO_CONNECTIONS"}
 if($NAWSERVER_DEMO_TX_BLOCK_BYTES -ne 1024){$suffix+="-blockbytes$NAWSERVER_DEMO_TX_BLOCK_BYTES"}
 if($NAWSERVER_DEMO_PSRAM -eq 'on'){$suffix+="-psram-$NAWSERVER_DEMO_PSRAM_PROFILE"}
 if($NAWSERVER_DEMO_MAIN_STACK -ne 8192){$suffix+="-mainstack$NAWSERVER_DEMO_MAIN_STACK"}
 if($NAWSERVER_DEMO_ASYNC_PROBE){$suffix+='-asyncprobe'}
 if($NAWSERVER_DEMO_STACK_DIAGNOSTICS){$suffix+='-stackdiag'}
 if($NAWSERVER_DEMO_MEMORY_PROBE){$suffix+='-memoryprobe'}
 if($NAWSERVER_DEMO_MEMORY_SAMPLER){$suffix+='-memorysampler'}
 if($NAWSERVER_DEMO_FULL_STOP_PROBE){$suffix+='-fullstop'}
 if($NAWSERVER_DEMO_TX_BULK_PROBE){$suffix+='-bulkprobe'}
 if($NAWSERVER_DEMO_INGRESS_SLOTS -ne 2){$suffix+="-ingress$NAWSERVER_DEMO_INGRESS_SLOTS"}
 if($NAWSERVER_DEMO_NATIVE_ACCEPT_DIAGNOSTICS){$suffix+='-nativeacceptdiag'}
 $BuildDirectory=Join-Path $PSScriptRoot ".build/$Target-$StatusProtocol$suffix"
}
if(-not [System.IO.Path]::IsPathRooted($BuildDirectory)){$BuildDirectory=Join-Path (Get-Location).ProviderPath $BuildDirectory}
$BuildDirectory=[System.IO.Path]::GetFullPath($BuildDirectory)
$existingCache=Join-Path $BuildDirectory 'CMakeCache.txt'
if(Test-Path -LiteralPath $existingCache){
 $cacheText=Get-Content -LiteralPath $existingCache -Raw
 foreach($placement in @(@('NAWSERVER_DEMO_TX_METADATA_MODE',$NAWSERVER_DEMO_TX_METADATA_MODE),@('NAWSERVER_DEMO_TX_PAYLOAD_PSRAM',$NAWSERVER_DEMO_TX_PAYLOAD_PSRAM),@('NAWSERVER_DEMO_TX_INTERNAL_ONLY',$NAWSERVER_DEMO_TX_INTERNAL_ONLY))){
  if($cacheText -notmatch ('(?m)^'+$placement[0]+':(?:STRING|UNINITIALIZED)='+$placement[1]+'\r?$')){
   throw 'This build cache predates or differs from the requested TX placement. Select a fresh -BuildDirectory.'
  }
 }
 if($NAWSERVER_DEMO_TX_METADATA_MODE -eq 1 -and $cacheText -notmatch ('(?m)^NAWSERVER_DEMO_TX_METADATA_STATIC_BYTES:(?:STRING|UNINITIALIZED)='+$NAWSERVER_DEMO_TX_METADATA_STATIC_BYTES+'\r?$')){throw 'This build cache has another static metadata arena size. Select a fresh -BuildDirectory.'}
}
$packages=Join-Path $env:USERPROFILE '.platformio/packages'
$env:IDF_PATH=$SdkRoot
$env:IDF_PYTHON_ENV_PATH=Join-Path $env:USERPROFILE '.platformio/penv/.espidf-5.5.6'
$env:PYTHONPATH="$env:IDF_PYTHON_ENV_PATH/Lib/site-packages"
$localEsptool=Join-Path $PSScriptRoot '../../../local/host-tools/esptool5/Lib/site-packages'
if(Test-Path $localEsptool){$env:PYTHONPATH+=';'+(Resolve-Path $localEsptool).Path}
$toolchain=if($Target -in @('esp32c3','esp32c6')){'toolchain-riscv32-esp@14.2.0+20260121'}else{'toolchain-xtensa-esp-elf'}
$env:PATH="$packages/$toolchain/bin;$packages/tool-ninja;$env:PATH"
$env:ESP_ROM_ELF_DIR="$packages/tool-esp-rom-elfs"
$env:IDF_COMPONENT_MANAGER='0'
$env:IDF_COMPONENT_CHECK_NEW_VERSION='0'
$defaults=Join-Path $PSScriptRoot 'sdkconfig.defaults'
$existingWorkerConfig=Join-Path $BuildDirectory 'sdkconfig'
if(Test-Path -LiteralPath $existingWorkerConfig){
 $existingStackConfig=Get-Content -LiteralPath $existingWorkerConfig -Raw
 if($existingStackConfig -notmatch '(?m)^CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192\r?$' -or $existingStackConfig -notmatch "(?m)^CONFIG_ESP_MAIN_TASK_STACK_SIZE=$NAWSERVER_DEMO_MAIN_STACK\r?$"){
  throw 'This build cache has a different HTTP or main task stack. Select a fresh -BuildDirectory.'
 }
}
if($Target -eq 'esp32s3'){
 $defaults+=';'+(Join-Path $PSScriptRoot 'sdkconfig.s3.defaults')
 $existingConfig=Join-Path $BuildDirectory 'sdkconfig'
 if(Test-Path -LiteralPath $existingConfig){
  $previousConfig=Get-Content -LiteralPath $existingConfig -Raw
  if($previousConfig -notmatch 'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.s3.csv"' -or $previousConfig -notmatch 'CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y'){
   throw 'This S3 build cache uses another Flash profile. Select a fresh -BuildDirectory for the shared 16 MiB S3 profile; never reuse its generated flash_args.'
  }
  if($previousConfig -notmatch '(?m)^CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192\r?$'){
   throw 'This S3 build cache has an unsafe old worker or main task stack. Select a fresh -BuildDirectory.'
  }
 }
}
if($NAWSERVER_DEMO_MAIN_STACK -eq 6144){$defaults+=';'+(Join-Path $PSScriptRoot 'sdkconfig.stack6144.defaults')}
if($StatusProtocol -eq 'ws'){$defaults+=";"+(Join-Path $PSScriptRoot 'sdkconfig.ws.defaults')}
if($Target -ne 'esp32'){$defaults+=';'+(Join-Path $PSScriptRoot 'sdkconfig.usb.defaults')}
if($NAWSERVER_DEMO_PSRAM -eq 'on'){
 $existingConfig=Join-Path $BuildDirectory 'sdkconfig'
 if((Test-Path -LiteralPath $existingConfig) -and ((Get-Content -LiteralPath $existingConfig -Raw) -notmatch '(?m)^CONFIG_SPIRAM=y\r?$')){
  throw 'This build cache does not have PSRAM enabled. Select a fresh -BuildDirectory.'
 }
 $psramDefaults=Join-Path $PSScriptRoot "sdkconfig.psram-$NAWSERVER_DEMO_PSRAM_PROFILE.defaults"
 if(Test-Path -LiteralPath $existingConfig){
  $previousConfig=Get-Content -LiteralPath $existingConfig -Raw
  foreach($setting in Get-Content -LiteralPath $psramDefaults){
   if($setting -match '^(CONFIG_[A-Z0-9_]+)=n$'){$expected='# '+$Matches[1]+' is not set'}
   elseif($setting -match '^CONFIG_[A-Z0-9_]+='){$expected=$setting}
   else{continue}
   if($previousConfig -notmatch ('(?m)^'+[regex]::Escape($expected)+'\r?$')){throw 'This build cache has a different PSRAM heap policy. Select a fresh -BuildDirectory.'}
  }
 }
 $defaults+=';'+$psramDefaults
}
elseif($Target -eq 'esp32s3'){
 $existingConfig=Join-Path $BuildDirectory 'sdkconfig'
 if((Test-Path -LiteralPath $existingConfig) -and ((Get-Content -LiteralPath $existingConfig -Raw) -match '(?m)^CONFIG_SPIRAM=y\r?$')){
  throw 'This build cache has PSRAM enabled. Select a fresh -BuildDirectory for the off profile.'
 }
}
if($NAWSERVER_DEMO_MEMORY_PROFILE -ne 'baseline'){
 $profileDefaults=Join-Path $PSScriptRoot "sdkconfig.$NAWSERVER_DEMO_MEMORY_PROFILE.defaults"
 $existingConfig=Join-Path $BuildDirectory 'sdkconfig'
 if(Test-Path -LiteralPath $existingConfig){
  $previousConfig=Get-Content -LiteralPath $existingConfig -Raw
  foreach($setting in Get-Content -LiteralPath $profileDefaults){
   if($setting -match '^CONFIG_[A-Z0-9_]+='){
    # ESP-IDF selects static Wi-Fi TX buffers with PSRAM, hiding the dynamic count.
    if($NAWSERVER_DEMO_PSRAM -eq 'on' -and $setting -match '^CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=' -and $previousConfig -match '(?m)^CONFIG_ESP_WIFI_STATIC_TX_BUFFER=y\r?$'){continue}
    if($previousConfig -notmatch ('(?m)^'+[regex]::Escape($setting)+'\r?$')){
     throw 'This build cache does not match the requested memory profile. Select a fresh -BuildDirectory.'
    }
   }
  }
 }
 $defaults+=';'+$profileDefaults
}
if($NAWSERVER_DEMO_MEMORY_TRACE -and -not $NAWSERVER_DEMO_MEMORY_BREAKDOWN){throw 'Memory tracing requires the breakdown harness.'}
if($NAWSERVER_DEMO_MEMORY_TRACE){$defaults+=';'+(Join-Path $PSScriptRoot 'sdkconfig.memory-trace.defaults')}
New-Item -ItemType Directory -Force $BuildDirectory | Out-Null
if($NAWSERVER_DEMO_README_PROFILE){$defaults+=';'+(Join-Path $PSScriptRoot "sdkconfig.readme-$NAWSERVER_DEMO_README_PROFILE.defaults")}
& cmake -S $PSScriptRoot -B $BuildDirectory -G Ninja "-DCMAKE_MAKE_PROGRAM=$packages/tool-ninja/ninja.exe" "-DIDF_TARGET=$Target" "-DNAWSERVER_DEMO_COMPACT_VERSION=$Version" "-DPYTHON=$env:IDF_PYTHON_ENV_PATH/Scripts/python.exe" -DPYTHON_DEPS_CHECKED=1 "-DNAWSERVER_DEMO_STATUS_WS=$([int]($StatusProtocol -eq 'ws'))" "-DNAWSERVER_DEMO_TX_MEMORY_MODE=$NAWSERVER_DEMO_TX_MEMORY_MODE" "-DNAWSERVER_DEMO_TX_EXECUTOR=$NAWSERVER_DEMO_TX_EXECUTOR" "-DNAWSERVER_DEMO_README_PROFILE=$NAWSERVER_DEMO_README_PROFILE" "-DNAWSERVER_DEMO_ASYNC_PROBE=$([int]$NAWSERVER_DEMO_ASYNC_PROBE.IsPresent)" "-DNAWSERVER_DEMO_TX_METADATA_MODE=$NAWSERVER_DEMO_TX_METADATA_MODE" "-DNAWSERVER_DEMO_TX_METADATA_STATIC_BYTES=$NAWSERVER_DEMO_TX_METADATA_STATIC_BYTES" "-DNAWSERVER_DEMO_TX_PAYLOAD_PSRAM=$NAWSERVER_DEMO_TX_PAYLOAD_PSRAM" "-DNAWSERVER_DEMO_TX_INTERNAL_ONLY=$NAWSERVER_DEMO_TX_INTERNAL_ONLY" "-DNAWSERVER_DEMO_TX_BLOCKS=$NAWSERVER_DEMO_TX_BLOCKS" "-DNAWSERVER_DEMO_TX_BLOCK_BYTES=$NAWSERVER_DEMO_TX_BLOCK_BYTES" "-DNAWSERVER_DEMO_HTTP_JOBS=$NAWSERVER_DEMO_HTTP_JOBS" "-DNAWSERVER_DEMO_CONNECTIONS=$NAWSERVER_DEMO_CONNECTIONS" "-DNAWSERVER_DEMO_INGRESS_SLOTS=$NAWSERVER_DEMO_INGRESS_SLOTS" "-DNAWSERVER_DEMO_NATIVE_ACCEPT_DIAGNOSTICS=$([int]$NAWSERVER_DEMO_NATIVE_ACCEPT_DIAGNOSTICS.IsPresent)" "-DNAWSERVER_DEMO_STACK_DIAGNOSTICS=$([int]$NAWSERVER_DEMO_STACK_DIAGNOSTICS.IsPresent)" "-DNAWSERVER_DEMO_MEMORY_PROBE=$([int]$NAWSERVER_DEMO_MEMORY_PROBE.IsPresent)" "-DNAWSERVER_DEMO_MEMORY_BREAKDOWN=$([int]$NAWSERVER_DEMO_MEMORY_BREAKDOWN.IsPresent)" "-DNAWSERVER_DEMO_MEMORY_LINK_PROFILE=$NAWSERVER_DEMO_MEMORY_LINK_PROFILE" "-DNAWSERVER_DEMO_MEMORY_SAMPLER=$([int]$NAWSERVER_DEMO_MEMORY_SAMPLER.IsPresent)" "-DNAWSERVER_DEMO_FULL_STOP_PROBE=$([int]$NAWSERVER_DEMO_FULL_STOP_PROBE.IsPresent)" "-DNAWSERVER_DEMO_TX_BULK_PROBE=$([int]$NAWSERVER_DEMO_TX_BULK_PROBE.IsPresent)" "-DSDKCONFIG=$BuildDirectory/sdkconfig" "-DSDKCONFIG_DEFAULTS=$defaults"
if($LASTEXITCODE){throw 'Compact ESP configuration failed'}
& cmake --build $BuildDirectory --parallel 6
if($LASTEXITCODE){throw 'Compact ESP build failed'}
