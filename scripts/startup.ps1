param([switch]$Remove)
$ErrorActionPreference = 'Stop'
$key = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
if ($Remove) {
  Remove-ItemProperty -LiteralPath $key -Name BrowserMonitor -ErrorAction SilentlyContinue
  Write-Host 'Browser Monitor automatic startup removed.'
  exit
}
$exe = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../build/host/browser-monitor.exe'))
if (!(Test-Path -LiteralPath $exe)) { throw 'Build the host first.' }
if (!(Test-Path -LiteralPath "$env:LOCALAPPDATA/BrowserMonitor/settings.dpapi")) {
  throw 'First run the host with --display, --signaling and --remember to save your preferred display and pairing.'
}
# Interactive sign-in is required for desktop capture; never run this in service Session 0.
New-Item -Path $key -Force | Out-Null
New-ItemProperty -LiteralPath $key -Name BrowserMonitor -PropertyType String -Value ('"' + $exe + '" --background') -Force | Out-Null
Write-Host 'Browser Monitor will start in your desktop session at Windows sign-in.'
