param([ValidateSet('Release', 'Debug')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
Set-Location $projectRoot
function Invoke-Checked([string]$Program, [string[]]$Arguments) {
  & $Program @Arguments
  if ($LASTEXITCODE -ne 0) { throw "$Program failed ($LASTEXITCODE)" }
}
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
if (!(Test-Path $vswhere)) { throw 'Install Visual Studio Build Tools with Desktop development with C++, MSVC Spectre-mitigated libraries and the Windows Driver Kit component.' }
$vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsPath) { throw 'Visual Studio C++ tools were not found.' }
Import-Module "$vsPath/Common7/Tools/Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
# WDK and SDK come from NuGet, pinned in driver/packages.config, the same way microsoft/Windows-driver-samples builds.
$nuget = '.deps/tools/nuget.exe'
if (!(Test-Path $nuget)) {
  New-Item -ItemType Directory -Force .deps/tools | Out-Null
  Invoke-WebRequest -UseBasicParsing https://dist.nuget.org/win-x86-commandline/latest/nuget.exe -OutFile $nuget
}
if ((Get-AuthenticodeSignature $nuget).Status -ne 'Valid') { throw "$nuget does not have a valid signature." }
Invoke-Checked $nuget @('restore', 'driver/packages.config', '-PackagesDirectory', '.deps/packages', '-NonInteractive')
# SignMode=Off: the WDK's auto-generated test certificate needs elevation. The catalog is still produced by Inf2Cat
# and is signed at install time by scripts/install-driver.ps1.
Invoke-Checked msbuild @('driver/BrowserMonitorIdd.sln', '-t:rebuild', '-m', '-nologo', '-warnaserror', '-clp:Verbosity=m', "-p:Configuration=$Configuration", '-p:Platform=x64', '-p:TargetVersion=Windows10', '-p:SignMode=Off')
Write-Host "Built driver package $projectRoot/driver/x64/$Configuration/BrowserMonitorIdd and driver/x64/$Configuration/BrowserMonitorIddApp.exe"
