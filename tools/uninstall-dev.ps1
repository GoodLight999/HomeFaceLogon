# tools/uninstall-dev.ps1
# Requires Administrator privileges

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Error "Please run this script as an Administrator."
    exit 1
}

$ErrorActionPreference = 'SilentlyContinue'

$installDir = "C:\Program Files\HomeFaceLogon"
$dataDir = "C:\ProgramData\HomeFaceLogon"
$clsid = "{5fd3d285-0dd9-4362-8855-e0abaacd4af6}"

# 1. Unregister Credential Provider
$cpPath = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Authentication\Credential Providers\$clsid"
if (Test-Path $cpPath) {
    Remove-Item -Path $cpPath -Recurse -Force | Out-Null
}

# 2. Unregister COM CLSID
$clsidPath = "HKLM:\SOFTWARE\Classes\CLSID\$clsid"
if (Test-Path $clsidPath) {
    Remove-Item -Path $clsidPath -Recurse -Force | Out-Null
}

# 3. Remove App Key
$appKeyPath = "HKLM:\SOFTWARE\HomeFaceLogon"
if (Test-Path $appKeyPath) {
    Remove-Item -Path $appKeyPath -Recurse -Force | Out-Null
}

# 4. Remove Files
if (Test-Path $installDir) {
    Remove-Item -Path $installDir -Recurse -Force | Out-Null
    if (Test-Path $installDir) {
        Write-Warning "Could not delete folder '$installDir'. The DLL might be currently loaded by LogonUI or Lsass. It will be deleted on next reboot."
    } else {
        Write-Host "Binaries removed successfully." -ForegroundColor Green
    }
}

# Delete runtime data (logs and configs)
if (Test-Path $dataDir) {
    Remove-Item -Path $dataDir -Recurse -Force | Out-Null
    Write-Host "Runtime data in $dataDir removed." -ForegroundColor Green
}

Write-Host "HomeFaceLogon uninstalled successfully." -ForegroundColor Green
