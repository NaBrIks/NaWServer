[CmdletBinding()]
param([string]$Profile='')
$ErrorActionPreference='Stop'
if([string]::IsNullOrWhiteSpace($Profile)) { throw 'Specify -Profile with your saved Windows Wi-Fi profile name' }
$privateFolder=Join-Path $PSScriptRoot '.private'
$exportFolder=Join-Path $privateFolder 'profile-export'
New-Item -ItemType Directory -Force -Path $exportFolder | Out-Null
$exportOutput=& netsh.exe wlan export profile "name=$Profile" "folder=$exportFolder" key=clear
if($LASTEXITCODE) { throw 'Windows Wi-Fi profile export failed' }
$file=Get-ChildItem -LiteralPath $exportFolder -Filter '*.xml' | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
if(-not $file) { throw 'Wi-Fi profile was not exported' }
try {
    [xml]$profileXml=Get-Content -LiteralPath $file.FullName -Raw
    $ssid=[string]$profileXml.WLANProfile.SSIDConfig.SSID.name
    $key=[string]$profileXml.WLANProfile.MSM.security.sharedKey.keyMaterial
    if(-not $ssid -or -not $key) { throw 'Saved SSID/key is unavailable' }
    function ConvertTo-CString([string]$Value) {
        '"'+$Value.Replace('\','\\').Replace('"','\"').Replace("`r",'\r').Replace("`n",'\n')+'"'
    }
    @('#pragma once',('#define WIFI_SSID '+(ConvertTo-CString $ssid)),
        ('#define WIFI_PASSWORD '+(ConvertTo-CString $key))) |
        Set-Content -LiteralPath (Join-Path $privateFolder 'wifi_config.h') -Encoding utf8NoBOM
    Write-Host "Prepared profile $ssid inside this demo; key is not logged."
} finally {
    Remove-Item -LiteralPath $file.FullName -Force
}
