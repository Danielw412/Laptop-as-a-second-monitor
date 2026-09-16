param([string]$Display,[int]$Seconds=30,[switch]$AllowPrimary,[switch]$LaptopMon)
# Runs the capture/convert/encode matrix with laptop-monitor-bench. Select the source with -LaptopMon (the virtual
# display found by identity) or -Display '\\.\DISPLAYn' (from laptop-monitor-bench --list).
$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot -Parent
Set-Location $projectRoot
if(!$LaptopMon -and !$Display){ throw 'Pass -LaptopMon or -Display \\.\DISPLAYn' }
New-Item -ItemType Directory -Force benchmarks/results | Out-Null
$stamp=Get-Date -Format 'yyyyMMdd-HHmmss'
foreach($backend in @('dxgi','wgc')) {
  foreach($mode in @('capture','convert','encode','capture-encode')) {
    $arguments=@('--capture',$backend,'--mode',$mode,'--seconds',"$Seconds",'--csv',"benchmarks/results/$stamp-$backend-$mode.csv")
    if($LaptopMon){$arguments+='--laptopmon'}else{$arguments+=@('--display',$Display)}
    if($AllowPrimary){$arguments+='--allow-primary'}
    & ./build/host/laptop-monitor-bench.exe @arguments > "benchmarks/results/$stamp-$backend-$mode.log" 2>&1
    if($LASTEXITCODE -ne 0){Write-Warning "$backend / $mode failed; inspect its log."}
  }
}
