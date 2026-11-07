param([Parameter(Mandatory=$true)][string]$ExpectedMac,
      [string]$Esptool='esptool',
      [int]$TimeoutSeconds=180,[switch]$Integration)
$ErrorActionPreference='Stop'
$repo=Resolve-Path (Join-Path $PSScriptRoot '../..')
$env:PYTHONUTF8='1'; $env:PYTHONIOENCODING='utf-8' # existing vendor CLI only
$ports=@(Get-CimInstance Win32_SerialPort | Where-Object PNPDeviceID -Like 'USB\VID_303A&PID_1001*')
if($ports.Count -ne 1){throw 'Identify the unique C3 first; ambiguous/missing USB device'}
$port=$ports[0].DeviceID
$ident=(& $Esptool --port $port chip-id 2>&1 | Out-String)
$identityExit=$LASTEXITCODE
$ident | Set-Content (Join-Path $repo 'local/sse-chip.log')
if($identityExit -or $ident -notmatch 'Chip type:\s+ESP32-C3' -or $ident -notmatch [regex]::Escape($ExpectedMac)){
    throw 'C3 identity mismatch; no flash write'
}
$device=if($Integration){'integration_device'}else{'device'}
$firmware=Join-Path $PSScriptRoot "$device/.pio/build/c3/firmware.bin"
if(!(Test-Path $firmware)){throw 'Build SSE firmware first'}
$length=(Get-Item $firmware).Length
if($length -lt 1 -or $length -gt 0x280000){throw 'Invalid app0 size'}
Get-FileHash $firmware -Algorithm SHA256
Write-Output ('App0 bytes {0}, write [0x10000,0x{1:x}); data partitions start 0x290000' -f $length,(0x10000+$length))
# Existing verified partition layout required. No backup, erase-all or data writes.
& $Esptool --chip esp32c3 --port $port --baud 460800 write-flash --no-progress 0x10000 $firmware
if($LASTEXITCODE){throw 'Flash failed'}
$logName=if($Integration){'local/sse-integration-device-serial.log'}else{'local/sse-device-serial.log'}
$output=Join-Path $repo $logName
$donePattern=if($Integration){'SSE_INTEGRATION_DONE [^\r\n]*[\r\n]'}else{'SSE_DONE [^\r\n]*[\r\n]'}
$serial=[IO.Ports.SerialPort]::new($port,115200)
$serial.DtrEnable=$false; $serial.RtsEnable=$false; $serial.ReadTimeout=500
$writer=$null; $done=$false; $tail=''
try {
    $serial.Open()
    $writer=[IO.StreamWriter]::new($output,$false,[Text.UTF8Encoding]::new($false))
    $deadline=(Get-Date).AddSeconds($TimeoutSeconds)
    while((Get-Date) -lt $deadline){
        $chunk=$serial.ReadExisting()
        if($chunk){
            $writer.Write($chunk); $writer.Flush(); $tail+=$chunk
            if($tail -match 'SSE_(DEVICE_)?FAIL|Guru Meditation|assert failed|task_wdt:|abort\(\) was called'){throw 'C3 test failed; see ignored serial log'}
            if($tail -match $donePattern){$done=$true; break}
            if($tail.Length -gt 5000){$tail=$tail.Substring($tail.Length-5000)}
        }
        Start-Sleep -Milliseconds 20
    }
}finally{
    if($writer){$writer.Dispose()}; $serial.Dispose()
}
if(!$done){throw 'No SSE_DONE before timeout; see ignored serial log'}
Get-Content $output | Where-Object {$_ -match '^SSE_'}
