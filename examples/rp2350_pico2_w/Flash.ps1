[CmdletBinding()]
param(
    [string]$DeviceSerial='',
    [string]$DeviceUrl='',
    [string]$Firmware=(Join-Path $PSScriptRoot 'build/nawserver_pico2_w.uf2'),
    [string]$Picotool=(Join-Path $env:USERPROFILE '.pico-sdk/picotool/2.3.1/picotool/picotool.exe')
)
$ErrorActionPreference='Stop'
if($DeviceSerial -notmatch '^[0-9A-Fa-f]{16}$') { throw 'Specify -DeviceSerial with the verified 16-digit USB serial of your RP2350' }
if(-not (Test-Path -LiteralPath $Firmware)) { throw 'Build this demo first' }
function Test-RomBootloader {
    # Device presence only. A damaged/unfinished application metadata block
    # must never prevent recovery of a chip that is already in ROM BOOTSEL.
    $device=Get-PnpDevice -PresentOnly -InstanceId "USB\VID_2E8A&PID_000F\$DeviceSerial" -ErrorAction SilentlyContinue
    $null -ne $device
}
$ready=Test-RomBootloader
if(-not $ready -and $DeviceUrl) {
    try {
        $authorization='Basic '+[Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes('demo:showcase-only'))
        $command=[Convert]::FromHexString('a166616374696f6e6a626f6f746c6f61646572')
        Invoke-RestMethod ($DeviceUrl.TrimEnd('/')+'/api/v2/control') -Method Post `
            -Headers @{Authorization=$authorization} -ContentType 'application/cbor' -Body $command -TimeoutSec 3 | Out-Null
        Write-Host "BOOTSEL requested over Wi-Fi: $DeviceUrl"
        Start-Sleep -Milliseconds 700
        $ready=Test-RomBootloader
    } catch { Write-Verbose 'HTTP reset unavailable; using exact-serial USB paths' }
}
if(-not $ready) {
    $ports=Get-CimInstance Win32_SerialPort | Where-Object { $_.PNPDeviceID -match 'VID_2E8A&PID_0009' }
    foreach($device in $ports) {
        $parent=Get-PnpDeviceProperty -InstanceId $device.PNPDeviceID -KeyName DEVPKEY_Device_Parent
        if($parent.Data -notlike "*\$DeviceSerial") { continue }
        # Windows CDC can temporarily fail Open after the previous monitor closes.
        for($openAttempt=0; $openAttempt -lt 20; ++$openAttempt) {
            $serial=[System.IO.Ports.SerialPort]::new($device.DeviceID,115200)
            $serial.DtrEnable=$true
            $serial.WriteTimeout=1000
            $sent=$false
            try { $serial.Open(); Start-Sleep -Milliseconds 100; $serial.Write('b'); $sent=$true; Start-Sleep -Milliseconds 200 }
            catch { Write-Verbose 'CDC is not ready; retrying before the vendor reset path' }
            finally { try { $serial.Dispose() } catch {} }
            if($sent) { break }
            Start-Sleep -Milliseconds 400
        }
    }
    for($attempt=0; $attempt -lt 60; ++$attempt) {
        Start-Sleep -Milliseconds 300
        if(Test-RomBootloader) { $ready=$true; break }
        $reset=& $Picotool reboot -u -f --ser $DeviceSerial 2>&1
    }
}
if(-not $ready) { throw 'This RP2350 is not responding to its software reset paths' }
# No separate verify/readback pass: only program the image and execute it.
for($usbAttempt=0; $usbAttempt -lt 6; ++$usbAttempt) {
    # PnP may publish ROM presence before WinUSB is ready. Retry only this
    # pre-programming discovery failure; an actual programming error stops.
    $programming=& $Picotool load -x $Firmware --ser $DeviceSerial 2>&1
    $programmingExit=$LASTEXITCODE
    $programming | Out-Host
    if($programmingExit -eq 0) { break }
    if(($programming | Out-String) -notmatch 'No accessible RP-series devices' -or $usbAttempt -eq 5) {
        throw 'Pico firmware programming failed'
    }
    Start-Sleep -Milliseconds 500
}
