#Requires -RunAsAdministrator
# Signs the unsigned package from scripts/build-driver.ps1 with a local self-signed certificate, trusts that certificate
# on this machine, and stages the driver. Run driver/x64/<Configuration>/BrowserMonitorIddApp.exe afterwards to
# create the virtual monitor. -Uninstall removes the driver package and the certificate again.
param([ValidateSet('Release', 'Debug')][string]$Configuration = 'Release', [switch]$Uninstall)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
function Invoke-Checked([string]$Program, [string[]]$Arguments) {
  & $Program @Arguments
  if ($LASTEXITCODE -ne 0) { throw "$Program failed ($LASTEXITCODE)" }
}
$subject = 'CN=Browser Monitor IDD Local Test Signing'
if ($Uninstall) {
  Get-WindowsDriver -Online | Where-Object { (Split-Path $_.OriginalFileName -Leaf) -eq 'browsermonitoridd.inf' } |
    ForEach-Object { Invoke-Checked pnputil @('/delete-driver', $_.Driver, '/uninstall', '/force') }
  Get-ChildItem Cert:\LocalMachine\My, Cert:\LocalMachine\Root, Cert:\LocalMachine\TrustedPublisher |
    Where-Object Subject -eq $subject | Remove-Item
  Write-Host 'Browser Monitor IDD driver package and signing certificate removed.'
  return
}
$package = "$projectRoot/driver/x64/$Configuration/BrowserMonitorIdd"
if (!(Test-Path "$package/BrowserMonitorIdd.cat")) { throw "Driver package not found. Run scripts/build-driver.ps1 -Configuration $Configuration first." }
$signtool = Get-ChildItem "$projectRoot/.deps/packages/Microsoft.Windows.SDK.CPP.*/c/bin/*/x64/signtool.exe" | Select-Object -Last 1
if (!$signtool) { throw 'signtool.exe not found. Run scripts/build-driver.ps1 first.' }
# Non-exportable key kept in the machine store; trusted only on this machine.
$cert = Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Subject -eq $subject -and $_.NotAfter -gt (Get-Date).AddDays(1) } | Select-Object -First 1
if (!$cert) {
  $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject -CertStoreLocation Cert:\LocalMachine\My -KeyExportPolicy NonExportable -NotAfter (Get-Date).AddYears(2)
}
foreach ($storeName in 'Root', 'TrustedPublisher') {
  $store = [Security.Cryptography.X509Certificates.X509Store]::new($storeName, 'LocalMachine')
  $store.Open('ReadWrite')
  $store.Add([Security.Cryptography.X509Certificates.X509Certificate2]::new($cert.RawData))
  $store.Close()
}
Invoke-Checked $signtool.FullName @('sign', '/fd', 'sha256', '/sm', '/s', 'My', '/sha1', $cert.Thumbprint, "$package/BrowserMonitorIdd.cat")
Invoke-Checked $signtool.FullName @('verify', '/pa', "$package/BrowserMonitorIdd.cat")
Invoke-Checked pnputil @('/add-driver', "$package/BrowserMonitorIdd.inf")
Write-Host "Driver staged. Create the monitor with: $projectRoot/driver/x64/$Configuration/BrowserMonitorIddApp.exe (elevated)"
