param([Parameter(Mandatory=$true)][string]$ExpectedMac,
      [string]$Port='COM10',
      [string]$Esptool='esptool')
$ErrorActionPreference='Stop'
$showcaseRoot=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$repo=(Resolve-Path (Join-Path $showcaseRoot '../..')).Path
$local=Join-Path $showcaseRoot 'local'
New-Item -ItemType Directory -Force -Path $local | Out-Null
$firmware=Join-Path $showcaseRoot '.pio/build/c3/firmware.bin'
$length=(Get-Item -LiteralPath $firmware).Length
if($length -le 0 -or $length -gt 0x280000){throw 'Image exceeds the verified app0 capacity'}
$actualTable=(Get-FileHash -LiteralPath (Join-Path $showcaseRoot '.pio/build/c3/partitions.bin')).Hash
$verifiedTable=(Get-FileHash -LiteralPath (Join-Path $repo 'examples/minimal/.pio/build/c3/partitions.bin')).Hash
if($actualTable -ne $verifiedTable){throw 'Layout differs from the previously verified minimal table; no write'}
$env:PYTHONUTF8='1';$env:PYTHONIOENCODING='utf-8' # Existing vendor CLI only.
$identity=(& $Esptool --port $Port chip-id 2>&1 | Out-String)
$exitCode=$LASTEXITCODE
$identity | Set-Content -LiteralPath (Join-Path $local 'c3-before-write.log')
if($exitCode -or $identity -notmatch 'Chip type:\s+ESP32-C3' -or $identity -notmatch [regex]::Escape($ExpectedMac)){throw 'Chip/MAC mismatch; no write'}
$hash=(Get-FileHash -LiteralPath $firmware -Algorithm SHA256).Hash.ToLowerInvariant()
$state=[ordered]@{port=$Port;firmware_sha256=$hash;firmware_bytes=$length;write_start='0x10000';write_end=('0x{0:x}' -f (0x10000+$length));partition_table_matches_verified=$true;partition_table_written=$false;data_partitions_written=$false;vendor_verified=$false;boot_ip_captured=$false}
$statePath=Join-Path $local 'c3-flash-state.json'
$state | ConvertTo-Json | Set-Content -LiteralPath $statePath
Write-Output ('C3 app0 only: {0} bytes, [0x10000,0x{1:x}); SHA256 {2}' -f $length,(0x10000+$length),$hash)
# No partition table, bootloader, NVS, data writes or flash backups.
& $Esptool --chip esp32c3 --port $Port --baud 460800 write-flash --no-progress 0x10000 $firmware *> (Join-Path $local 'c3-flash.log')
if($LASTEXITCODE){throw 'Vendor flash failed; see ignored local log'}
if([IO.File]::ReadAllText((Join-Path $local 'c3-flash.log')) -notmatch 'Hash of data verified'){throw 'No vendor hash verification'}
$state.vendor_verified=$true;$state | ConvertTo-Json | Set-Content -LiteralPath $statePath
$serial=[IO.Ports.SerialPort]::new($Port,115200);$serial.DtrEnable=$false;$serial.RtsEnable=$false;$serial.ReadTimeout=500
$content='';$readyAt=$null
try{
 $serial.Open();$deadline=(Get-Date).AddSeconds(80)
 while((Get-Date)-lt $deadline){
  $chunk=$serial.ReadExisting()
  if($chunk){$content+=$chunk;[IO.File]::WriteAllText((Join-Path $local 'c3-boot.log'),$content)
   if($content -match 'Guru Meditation|assert failed|Stack canary|abort\(\) was called'){throw 'Boot failure; inspect ignored serial log'}
   if(!$readyAt -and $content -match 'IP: \d+\.\d+\.\d+\.\d+'){$readyAt=Get-Date}
  }
  if($readyAt -and ((Get-Date)-$readyAt).TotalSeconds -ge 3){break}
  Start-Sleep -Milliseconds 30
 }
}finally{$serial.Dispose()}
$ip=[regex]::Match($content,'IP: (\d+\.\d+\.\d+\.\d+)')
if(!$ip.Success){throw 'No fresh IP captured; inspect ignored boot log'}
$state.ip=$ip.Groups[1].Value;$state.boot_ip_captured=$true
$state | ConvertTo-Json | Set-Content -LiteralPath $statePath
Write-Output 'Vendor hash verification PASS; Showcase boot/IP PASS; serial released.'
