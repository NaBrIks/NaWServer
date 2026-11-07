param([string]$ZigPath=(Join-Path $PSScriptRoot '../../../local/host-tools/zig-x86_64-windows-0.14.1/zig.exe'))
$ErrorActionPreference='Stop'
$showcaseRoot=Split-Path $PSScriptRoot -Parent
$vendor=Join-Path $showcaseRoot 'managed_components\espressif__cbor\tinycbor\src'
if(!(Test-Path $vendor)){throw 'Run the C3 configure/build first to resolve pinned TinyCBOR.'}
if(!(Test-Path $ZigPath)){throw 'Pass -ZigPath pointing to an existing Zig C/C++ compiler. This script installs nothing.'}
$build=Join-Path $showcaseRoot 'local\native-tests';New-Item -ItemType Directory -Path $build -Force|Out-Null
$env:ZIG_GLOBAL_CACHE_DIR=Join-Path $build 'zig-global-cache'
$sources=@('cborencoder.c','cborencoder_close_container_checked.c','cborparser.c','cborparser_float.c','cborencoder_float.c','cborvalidation.c','cborerrorstrings.c')|ForEach-Object{Join-Path $vendor $_}
Push-Location $showcaseRoot
try {
 $previousOutput=Join-Path $build 'device.cbor';if(Test-Path -LiteralPath $previousOutput){Remove-Item -LiteralPath $previousOutput}
 & dotnet run --project simulator/Showcase.Tests -- --interop $build
 if($LASTEXITCODE){throw 'fixture generation failed'}
 Push-Location $build
 try {
  foreach($source in $sources){
   & $ZigPath cc -std=c11 -O1 -I $vendor -c $source -o ([IO.Path]::GetFileNameWithoutExtension($source)+'.obj')
   if($LASTEXITCODE){throw 'vendor C build failed'}
  }
  $objects=Get-ChildItem $build -Filter '*.obj'|ForEach-Object FullName
  & $ZigPath c++ -std=c++17 -O1 -Wall -Wextra -I $vendor -I (Join-Path $showcaseRoot 'main') (Join-Path $showcaseRoot 'tests\wire_test.cpp') (Join-Path $showcaseRoot 'main\wire.cpp') @objects -o wire_test.exe
  if($LASTEXITCODE){throw 'C++ test build failed'}
  & .\wire_test.exe $build
  if($LASTEXITCODE){throw 'C++ tests failed'}
 }finally{Pop-Location}
 & dotnet run --no-build --project simulator/Showcase.Tests -- --interop $build
 if($LASTEXITCODE){throw 'reverse interop failed'}
}finally{Pop-Location}

