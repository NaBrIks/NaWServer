$ErrorActionPreference='Stop'
$record=Join-Path $PSScriptRoot 'local\host-process.json'
if(!(Test-Path -LiteralPath $record)){Write-Output 'No recorded showcase host. Stop Visual Studio or dotnet run in its own terminal if applicable.';return}
$saved=Get-Content -LiteralPath $record -Raw | ConvertFrom-Json -DateKind String
$process=Get-Process -Id $saved.pid -ErrorAction SilentlyContinue
if(!$process){Write-Output 'Recorded showcase process is no longer running.';return}
if($process.Path -ne $saved.path -or $process.StartTime.ToString('O') -ne $saved.start_time){throw 'PID identity changed; refusing to stop an unrelated process'}
Stop-Process -Id $process.Id
Write-Output "Stopped only showcase host PID $($process.Id)."

