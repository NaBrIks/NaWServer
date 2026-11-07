param([Parameter(Mandatory=$true)][string]$Cxx,
      [string[]]$CompilerPrefix=@())
$ErrorActionPreference='Stop'
$repo=Resolve-Path (Join-Path $PSScriptRoot '../..')
$out=Join-Path $repo 'local/url-decode-host'
New-Item -ItemType Directory -Force $out | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR=Join-Path $out 'zig-cache'
& $Cxx @CompilerPrefix -std=c++17 -O2 -Wall -Wextra -Werror -pedantic (Join-Path $PSScriptRoot 'decoder.cpp') (Join-Path $PSScriptRoot 'correctness.cpp') -o (Join-Path $out 'correctness.exe')
if($LASTEXITCODE){throw 'Host build failed'}
& (Join-Path $out 'correctness.exe')
if($LASTEXITCODE){throw 'Correctness failed'}
