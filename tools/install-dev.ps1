# tools/install-dev.ps1
# Requires Administrator privileges

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Error "Please run this script as an Administrator."
    exit 1
}

$ErrorActionPreference = 'Stop'

$installDir = "C:\Program Files\HomeFaceLogon"
$dataDir = "C:\ProgramData\HomeFaceLogon"
$clsid = "{5fd3d285-0dd9-4362-8855-e0abaacd4af6}"

# 1. Determine build source path
$dllSource = Join-Path $PSScriptRoot "..\x64\Release\FaceLogonProvider.dll"
if (-not (Test-Path $dllSource)) {
    $dllSource = Join-Path $PSScriptRoot "..\x64\Debug\FaceLogonProvider.dll"
}

if (-not (Test-Path $dllSource)) {
    Write-Error "Could not find FaceLogonProvider.dll. Please build the solution first."
    exit 1
}

# 2. Copy binaries
New-Item -ItemType Directory -Path $installDir -Force | Out-Null
Copy-Item -Path $dllSource -Destination (Join-Path $installDir "FaceLogonProvider.dll") -Force

$exeSource = Join-Path $PSScriptRoot "..\x64\Release\FaceLogonSetup.exe"
if (-not (Test-Path $exeSource)) {
    $exeSource = Join-Path $PSScriptRoot "..\x64\Debug\FaceLogonSetup.exe"
}
if (Test-Path $exeSource) {
    Copy-Item -Path $exeSource -Destination (Join-Path $installDir "FaceLogonSetup.exe") -Force
}

# 3. Create settings and logs directories
New-Item -ItemType Directory -Path $dataDir -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $dataDir "logs") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $dataDir "models") -Force | Out-Null

# 4. Set directory ACL
$acl = Get-Acl -Path $dataDir
$acl.SetAccessRuleProtection($true, $false)
$systemSid = New-Object System.Security.Principal.SecurityIdentifier([System.Security.Principal.WellKnownSidType]::LocalSystemSid, $null)
$adminsSid = New-Object System.Security.Principal.SecurityIdentifier([System.Security.Principal.WellKnownSidType]::BuiltinAdministratorsSid, $null)

$systemRule = New-Object System.Security.AccessControl.FileSystemAccessRule($systemSid, "FullControl", "ContainerInherit, ObjectInherit", "None", "Allow")
$adminsRule = New-Object System.Security.AccessControl.FileSystemAccessRule($adminsSid, "FullControl", "ContainerInherit, ObjectInherit", "None", "Allow")

$acl.AddAccessRule($systemRule)
$acl.AddAccessRule($adminsRule)
Set-Acl -Path $dataDir -AclObject $acl

# 5. Register COM CLSID
$clsidPath = "HKLM:\SOFTWARE\Classes\CLSID\$clsid"
if (-not (Test-Path $clsidPath)) {
    New-Item -Path $clsidPath -Force | Out-Null
}
Set-ItemProperty -Path $clsidPath -Name "(Default)" -Value "HomeFaceLogon Credential Provider" -Force
$inprocPath = Join-Path $clsidPath "InprocServer32"
if (-not (Test-Path $inprocPath)) {
    New-Item -Path $inprocPath -Force | Out-Null
}
Set-ItemProperty -Path $inprocPath -Name "(Default)" -Value (Join-Path $installDir "FaceLogonProvider.dll") -Force
Set-ItemProperty -Path $inprocPath -Name "ThreadingModel" -Value "Apartment" -Force

# 6. Register Credential Provider
$cpPath = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Authentication\Credential Providers\$clsid"
if (-not (Test-Path $cpPath)) {
    New-Item -Path $cpPath -Force | Out-Null
}
Set-ItemProperty -Path $cpPath -Name "(Default)" -Value "HomeFaceLogon Credential Provider" -Force

# 7. Initialize Application Key (Disabled by default)
$appKeyPath = "HKLM:\SOFTWARE\HomeFaceLogon"
if (-not (Test-Path $appKeyPath)) {
    New-Item -Path $appKeyPath -Force | Out-Null
}
Set-ItemProperty -Path $appKeyPath -Name "Enabled" -Value 0 -Type DWord -Force

Write-Host "HomeFaceLogon installed and registered successfully." -ForegroundColor Green
Write-Host "Note: The provider is initially DISABLED. Use tools/enable-provider.ps1 to enable it." -ForegroundColor Yellow
