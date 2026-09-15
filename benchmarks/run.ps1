param([Parameter(Mandatory)][string]$Display,[int]$Seconds=30,[switch]$AllowPrimary)
$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot -Parent
Set-Location $projectRoot
New-Item -ItemType Directory -Force benchmarks/results | Out-Null
$stamp=Get-Date -Format 'yyyyMMdd-HHmmss'
foreach($backend in @('dxgi','wgc')) {
  foreach($mode in @('capture','convert','encode','capture-encode')) {
    $arguments=@('--display',$Display,'--capture',$backend,'--mode',$mode,'--seconds',"$Seconds",'--csv',"benchmarks/results/$stamp-$backend-$mode.csv")
    if($AllowPrimary){$arguments+='--allow-primary'}
    & ./build/host/browser-monitor.exe @arguments > "benchmarks/results/$stamp-$backend-$mode.log" 2>&1
    if($LASTEXITCODE -ne 0){Write-Warning "$backend / $mode failed; inspect its log."}
  }
}
