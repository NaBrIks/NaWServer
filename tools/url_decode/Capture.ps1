param([Parameter(Mandatory=$true)][string]$Port,
      [string]$OutputPath='', [int]$TimeoutSeconds=240)
$ErrorActionPreference='Stop'
$repo=Resolve-Path (Join-Path $PSScriptRoot '../..')
if(!$OutputPath){$OutputPath=Join-Path $repo 'local/url-decode-serial.log'}
New-Item -ItemType Directory -Force (Split-Path $OutputPath) | Out-Null
$serial=[IO.Ports.SerialPort]::new($Port,115200)
$serial.DtrEnable=$false; $serial.RtsEnable=$false
$serial.ReadTimeout=500
$serial.Open()
$writer=[IO.StreamWriter]::new($OutputPath,$false,[Text.UTF8Encoding]::new($false))
$tail='';$done=$false
try {
    $deadline=(Get-Date).AddSeconds($TimeoutSeconds)
    while((Get-Date)-lt $deadline){
        $chunk=$serial.ReadExisting()
        if($chunk){
            $writer.Write($chunk);$writer.Flush();$tail+=$chunk
            if($tail -match 'ABORT correctness'){throw 'Device correctness failed; see ignored serial log'}
            if($tail -match 'DONE checksum=[^\r\n]*[\r\n]'){ $done=$true; break }
            if($tail.Length -gt 2000){$tail=$tail.Substring($tail.Length-2000)}
        }
        Start-Sleep -Milliseconds 30
    }
}finally{$writer.Dispose();$serial.Dispose()}
if(!$done){throw 'No DONE marker; capture timed out'}
Get-Content $OutputPath | Where-Object {$_ -match '^(META|CORRECTNESS|DONE|APP_ELF|UTF8_CHECK)'}
