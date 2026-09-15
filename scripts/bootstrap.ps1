$ErrorActionPreference = 'Stop'
foreach ($dependency in @(@{Command='git';Id='Git.Git'},@{Command='node';Id='OpenJS.NodeJS.LTS'},@{Command='cmake';Id='Kitware.CMake'},@{Command='python';Id='Python.Python.3.13'})) {
  if (!(Get-Command $dependency.Command -ErrorAction SilentlyContinue)) {
    & winget install --id $dependency.Id --exact --silent --accept-package-agreements --accept-source-agreements
    if ($LASTEXITCODE -ne 0) { throw "Install failed: $($dependency.Id)" }
  }
}
Write-Host 'Dependencies available. If anything was newly installed, open a new PowerShell before building.'
Set-Location (Split-Path $PSScriptRoot -Parent)
& npm ci
if ($LASTEXITCODE -ne 0) { throw 'npm ci failed' }
& "$PSScriptRoot/build-host.ps1"
