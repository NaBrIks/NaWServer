param([Parameter(Mandatory=$true)][string]$ExpectedMac,
      [string]$Port='COM10',
      [string]$FirmwarePath='',
      [string]$Esptool='esptool')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$env:PYTHONUTF8='1'; $env:PYTHONIOENCODING='utf-8'
$firmware=if($FirmwarePath){(Resolve-Path -LiteralPath $FirmwarePath).Path}else{Join-Path $repo 'examples/minimal/.pio/build/c3/firmware.bin'}
if(!(Test-Path -LiteralPath $firmware) -or (Get-Item -LiteralPath $firmware).Length -gt 0x280000){throw 'Missing/oversized app0 firmware'}
$identity=(& $Esptool --port $Port chip-id 2>&1 | Out-String)
$identity | Set-Content (Join-Path $repo 'local/http-api-chip.log')
if($LASTEXITCODE -or $identity -notmatch 'Chip type:\s+ESP32-C3' -or $identity -notmatch [regex]::Escape($ExpectedMac)) { throw 'Chip/MAC mismatch; no flash write performed' }
$hash=(Get-FileHash -LiteralPath $firmware -Algorithm SHA256).Hash.ToLowerInvariant()
& $Esptool --chip esp32c3 --port $Port --baud 460800 write-flash --no-progress 0x10000 $firmware *> (Join-Path $repo 'local/http-api-flash.log')
if($LASTEXITCODE){throw 'Flash failed'}
$serial=[IO.Ports.SerialPort]::new($Port,115200)
$serial.DtrEnable=$false; $serial.RtsEnable=$false; $serial.ReadTimeout=500
$serial.Open()
$writer=[IO.StreamWriter]::new((Join-Path $repo 'local/http-api-serial.log'),$false,[Text.UTF8Encoding]::new($false))
$content=''
try {
    $deadline=(Get-Date).AddSeconds(100)
    while((Get-Date)-lt $deadline){
        $chunk=$serial.ReadExisting()
        if($chunk){$writer.Write($chunk);$writer.Flush();$content+=$chunk}
        if($content -match 'FAIL response|Guru Meditation|panic|Stack canary|assert failed'){throw 'Device failure; see ignored serial log'}
        if($content -match 'HTTP server ready' -and $content -match 'PASS: \d+ response/basic checks' -and $content -match 'RESULT passed=\d+ failed=0'){break}
        Start-Sleep -Milliseconds 30
    }
} finally { $writer.Dispose();$serial.Dispose() }
$response=[regex]::Match($content,'PASS: (\d+) response/basic checks; Response=(\d+) bytes')
$selftest=[regex]::Match($content,'RESULT passed=(\d+) failed=(\d+) free_heap=(\d+)')
$ip=[regex]::Match($content,'IP: (\d+\.\d+\.\d+\.\d+)')
$state=[ordered]@{port=$Port;firmware_sha256=$hash;ip=$ip.Groups[1].Value;response_checks=$response.Groups[1].Value;response_bytes=$response.Groups[2].Value;loopback_pass=$selftest.Groups[1].Value;loopback_failed=$selftest.Groups[2].Value;free_heap=$selftest.Groups[3].Value;http_ready=($content -match 'HTTP server ready')}
$state | ConvertTo-Json | Set-Content (Join-Path $repo 'local/http-api-state.json')
$state | ConvertTo-Json
if(!$response.Success -or !$selftest.Success -or !$ip.Success -or $selftest.Groups[2].Value -ne '0'){throw 'Incomplete HTTP C3 evidence'}
