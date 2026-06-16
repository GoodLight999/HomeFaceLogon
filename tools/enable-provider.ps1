# tools/enable-provider.ps1
# Requires Administrator privileges

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Error "Please run this script as an Administrator."
    exit 1
}

$appKeyPath = "HKLM:\SOFTWARE\HomeFaceLogon"
if (-not (Test-Path $appKeyPath)) {
    New-Item -Path $appKeyPath -Force | Out-Null
}
Set-ItemProperty -Path $appKeyPath -Name "Enabled" -Value 1 -Type DWord -Force
Write-Host "HomeFaceLogon Credential Provider has been ENABLED." -ForegroundColor Green
