param([switch]$Rebuild)
$ErrorActionPreference='Stop'
$showcaseRoot=$PSScriptRoot
$local=Join-Path $showcaseRoot 'local'
New-Item -ItemType Directory -Force -Path $local | Out-Null
$project=Join-Path $showcaseRoot 'simulator\Showcase.Host'
$executable=Join-Path $project 'bin\Debug\net10.0\Showcase.Host.exe'
$listener=Get-NetTCPConnection -LocalAddress 127.0.0.1 -LocalPort 8788 -State Listen -ErrorAction SilentlyContinue
if($listener){throw 'Port 8788 is already in use. Existing services are not stopped. If this is the demo, use Stop-Showcase.ps1 first.'}
if($Rebuild -or !(Test-Path -LiteralPath $executable) -or !(Test-Path -LiteralPath (Join-Path $showcaseRoot 'frontend\dist\asset-manifest.json'))){
    Push-Location $showcaseRoot
    try{
        Push-Location (Join-Path $showcaseRoot 'frontend')
        try{ & npm.cmd ci --prefix ../../.. --no-audit --no-fund; if($LASTEXITCODE -ne 0){throw 'npm ci failed'}; & npm.cmd run build; if($LASTEXITCODE -ne 0){throw 'frontend build failed'} }finally{Pop-Location}
        & dotnet build (Join-Path $showcaseRoot 'simulator\Showcase.sln') --nologo
        if($LASTEXITCODE -ne 0){throw '.NET build failed'}
        & dotnet run --no-build --project (Join-Path $showcaseRoot 'simulator\Showcase.Assets') -- (Join-Path $showcaseRoot 'frontend\dist')
        if($LASTEXITCODE -ne 0){throw 'Asset pack failed'}
    }finally{Pop-Location}
}
$process=Start-Process -FilePath $executable -WorkingDirectory $project -WindowStyle Hidden -RedirectStandardOutput (Join-Path $local 'host.out.log') -RedirectStandardError (Join-Path $local 'host.err.log') -PassThru
@{pid=$process.Id;start_time=$process.StartTime.ToString('O');path=$executable} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $local 'host-process.json') -Encoding utf8
$ready=$false
for($attempt=0;$attempt -lt 20;$attempt++){
    Start-Sleep -Milliseconds 250
    if($process.HasExited){throw 'Demo host exited; inspect local/host.err.log'}
    try{$health=Invoke-WebRequest -Uri 'http://127.0.0.1:8788/health' -TimeoutSec 1;if($health.StatusCode -eq 200 -and $health.Headers['X-Showcase-Source'] -eq 'simulator'){$ready=$true;break}}catch{}
}
if(!$ready){throw "Host process $($process.Id) started but readiness is unconfirmed; inspect local logs"}
Write-Output "Showcase ready: http://127.0.0.1:8788 (PID $($process.Id)). Loopback only; no controller connected."


