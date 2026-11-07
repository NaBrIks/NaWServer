param([Parameter(Mandatory=$true)][string]$ExpectedMac,
      [string]$Esptool='esptool',
      [string]$FirmwarePath='')
$ErrorActionPreference='Stop'
$env:PYTHONUTF8='1'; $env:PYTHONIOENCODING='utf-8' # vendor CLI on redirected Windows stdout
$repo=Resolve-Path (Join-Path $PSScriptRoot '../..')
$ports=@(Get-CimInstance Win32_SerialPort | Where-Object PNPDeviceID -Like 'USB\VID_303A&PID_1001*')
if($ports.Count -ne 1){throw 'Exactly one Espressif USB Serial/JTAG device required; identify manually'}
$port=$ports[0].DeviceID
$ident=(& $Esptool --port $port chip-id 2>&1 | Out-String)
$ident | Set-Content (Join-Path $repo 'local/url-decode-chip.log')
if($LASTEXITCODE -or $ident -notmatch 'Chip type:\s+ESP32-C3' -or $ident -notmatch [regex]::Escape($ExpectedMac)){
    throw 'Target chip/device identity mismatch; no write performed'
}
$firmware=if($FirmwarePath){(Resolve-Path -LiteralPath $FirmwarePath).Path}else{Join-Path $PSScriptRoot 'device/.pio/build/c3/firmware.bin'}
if(!(Test-Path $firmware) -or (Get-Item $firmware).Length -gt 0x280000){throw 'Missing/oversized app'}
Get-FileHash $firmware -Algorithm SHA256
# Owner 2026-10-19 explicitly removed backups. Never read/create a flash backup.
# App0 only; no erase-all, bootloader/partition/NVS/data writes.
& $Esptool --chip esp32c3 --port $port --baud 460800 write-flash --no-progress 0x10000 $firmware
if($LASTEXITCODE){throw 'Flash failed'}
Write-Output "Flashed identified C3 on $port; immediately run Capture.ps1 -Port $port"
