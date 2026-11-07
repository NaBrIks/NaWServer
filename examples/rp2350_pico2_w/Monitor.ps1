[CmdletBinding()]
param(
    [string]$Port, [int]$Seconds = 60, [string]$Send = 'r', [string]$LogPath,
    [string]$DeviceSerial = ''
)
$ErrorActionPreference = 'Stop'
if (-not $Port) {
    if ($DeviceSerial -notmatch '^[0-9A-Fa-f]{16}$') { throw 'Specify -Port or the verified RP2350 -DeviceSerial' }
    $serialDevice = Get-CimInstance Win32_SerialPort | Where-Object {
        if ($_.PNPDeviceID -notmatch 'VID_2E8A') { return $false }
        $parent = Get-PnpDeviceProperty -InstanceId $_.PNPDeviceID -KeyName DEVPKEY_Device_Parent
        $parent.Data -like "*\$DeviceSerial"
    } | Select-Object -First 1
    if (-not $serialDevice) { throw 'Pico USB serial port not found; specify -Port' }
    $Port = $serialDevice.DeviceID
}
$serial = [System.IO.Ports.SerialPort]::new($Port, 115200)
$serial.DtrEnable = $true
$serial.ReadTimeout = 200
$log = [System.Text.StringBuilder]::new()
try {
    for($openAttempt=0; ; ++$openAttempt) {
        try { $serial.Open(); break }
        catch {
            if($openAttempt -ge 19) { throw }
            try { $serial.Dispose() } catch {}
            Start-Sleep -Milliseconds 400
            $serial=[System.IO.Ports.SerialPort]::new($Port,115200)
            $serial.DtrEnable=$true
            $serial.ReadTimeout=200
        }
    }
    if ($Send) { $serial.Write($Send) }
    $deadline = [DateTime]::UtcNow.AddSeconds($Seconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        try { $chunk = $serial.ReadExisting() }
        catch { Write-Host 'Pico USB disconnected'; break }
        if ($chunk) { Write-Host -NoNewline $chunk; [void]$log.Append($chunk) }
        Start-Sleep -Milliseconds 100
    }
} finally {
    if ($LogPath) { [System.IO.File]::WriteAllText($LogPath, $log.ToString()) }
    try { $serial.Dispose() } catch { Write-Warning 'Pico USB connection closed during capture' }
}
