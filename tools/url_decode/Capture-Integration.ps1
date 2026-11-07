param([Parameter(Mandatory=$true)][string]$Port,
      [Parameter(Mandatory=$true)][ValidateSet('lookup','from_chars')][string]$Backend,
      [Parameter(Mandatory=$true)][string]$OutputPath,[int]$TimeoutSeconds=180)
$ErrorActionPreference='Stop'
$serial=[IO.Ports.SerialPort]::new($Port,115200)
$serial.DtrEnable=$false;$serial.RtsEnable=$false;$serial.ReadTimeout=500
$serial.Open();$writer=[IO.StreamWriter]::new($OutputPath,$false,[Text.UTF8Encoding]::new($false))
$all='';$done=$false
try {
    $deadline=(Get-Date).AddSeconds($TimeoutSeconds)
    while((Get-Date)-lt $deadline){
        $chunk=$serial.ReadExisting()
        if($chunk){
            $all+=$chunk;$writer.Write($chunk);$writer.Flush()
            if($all -match 'RESULT passed=\d+ failed=\d+ free_heap=\d+\r?\n'){$done=$true;break}
        }
        Start-Sleep -Milliseconds 30
    }
}finally{$writer.Dispose();$serial.Dispose()}
if(!$done){throw 'No integration RESULT; inspect ignored log'}
if($all -match 'task_wdt:|Guru Meditation|Stack canary|PARAM_FAIL|selftest: FAIL|RESULT passed=\d+ failed=[1-9]'){
    throw 'Hardware integration failed; inspect ignored log'
}
if($all -notmatch ('PARAM_TEST backend='+[regex]::Escape($Backend)+' checks=\d+ failed=0') -or
   $all -notmatch ('decoder='+[regex]::Escape($Backend)+' cplusplus=201703')) {throw 'Missing selected C++17 backend confirmation'}
$all -split "`r?`n" | Where-Object {$_ -match 'PARAM_TEST|selftest: (decoder=|RESULT|FAIL)'}
