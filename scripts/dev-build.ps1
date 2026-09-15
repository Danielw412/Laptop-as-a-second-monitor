# Developer convenience: enter the VS dev shell and build (optionally configure) a CMake tree.
param([string]$BuildDir = 'build/host', [switch]$Configure, [string[]]$Targets = @(), [int]$Jobs = 8, [string]$LogFile)
$ErrorActionPreference = 'Continue'
$projectRoot = Split-Path $PSScriptRoot -Parent
Set-Location $projectRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
Import-Module "$vsPath\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' 2>$null | Out-Null
Set-Location $projectRoot
if ($Configure) {
  cmake -S . -B $BuildDir -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$projectRoot/.deps/install"
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
$arguments = @('--build', $BuildDir, '-j', "$Jobs")
if ($Targets.Count) { $arguments += '--target'; $arguments += $Targets }
$arguments += @('--', '-k', '0')
if ($LogFile) { cmake @arguments 2>&1 | Tee-Object -FilePath $LogFile | Out-Null } else { cmake @arguments }
exit $LASTEXITCODE
