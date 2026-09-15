param([switch]$SkipDependencies, [int]$Jobs = 8)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
Set-Location $projectRoot
function Invoke-Checked([string]$Program, [string[]]$Arguments) {
  & $Program @Arguments
  if ($LASTEXITCODE -ne 0) { throw "$Program failed ($LASTEXITCODE)" }
}
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
if (!(Test-Path $vswhere)) { throw 'Install Visual Studio Build Tools with Desktop development with C++ and Windows SDK.' }
$vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsPath) { throw 'Visual Studio C++ tools were not found.' }
Import-Module "$vsPath/Common7/Tools/Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
if (!$SkipDependencies) {
  $deps = @(
    @{Name='libdatachannel'; Url='https://github.com/paullouisageneau/libdatachannel.git'; Tag='v0.24.5'; Commit='443f6934d9007eb7076ab7825ba330f355fcbead'},
    @{Name='mbedtls'; Url='https://github.com/Mbed-TLS/mbedtls.git'; Tag='mbedtls-3.6.7'; Commit='068ff080b369adfac81509f9b57b2afabaf82dc5'}
  )
  foreach ($dep in $deps) {
    $path = ".deps/$($dep.Name)"
    if (!(Test-Path "$path/CMakeLists.txt")) { Invoke-Checked git @('clone','--depth','1','--branch',$dep.Tag,'--recurse-submodules','--shallow-submodules',$dep.Url,$path) }
    $actual = & git -C $path rev-parse HEAD
    if ($actual -ne $dep.Commit) { throw "Unexpected dependency revision in $path. Expected $($dep.Commit)." }
    Invoke-Checked git @('-C',$path,'submodule','update','--init','--recursive','--depth','1')
  }
  # Official Mbed TLS config tool; DTLS-SRTP is required for encrypted WebRTC media.
  Invoke-Checked python @('.deps/mbedtls/scripts/config.py','set','MBEDTLS_SSL_DTLS_SRTP')
  Invoke-Checked cmake @('-S','.deps/mbedtls','-B','build/tls','-G','Ninja','-DCMAKE_BUILD_TYPE=Release','-DENABLE_TESTING=OFF','-DENABLE_PROGRAMS=OFF',"-DCMAKE_INSTALL_PREFIX=$projectRoot/.deps/install")
  Invoke-Checked cmake @('--build','build/tls','-j',"$Jobs")
  Invoke-Checked cmake @('--install','build/tls')
}
Invoke-Checked cmake @('-S','.','-B','build/host','-G','Ninja','-DCMAKE_BUILD_TYPE=Release',"-DCMAKE_PREFIX_PATH=$projectRoot/.deps/install")
Invoke-Checked cmake @('--build','build/host','-j',"$Jobs")
Invoke-Checked ctest @('--test-dir','build/host','--output-on-failure')
Write-Host "Built $projectRoot/build/host/browser-monitor.exe"
