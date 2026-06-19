#
# HomeFaceLogon - Build Installer Script
#

$iscc = "C:\Program Files (x86)\Inno Setup 6\iscc.exe"
if (!(Test-Path $iscc)) {
    $iscc = (Get-Command iscc -ErrorAction SilentlyContinue).Source
}

if (!$iscc) {
    Write-Error "Inno Setup 6 is not installed or iscc.exe is not in PATH."
    Write-Host "Please download and install Inno Setup 6 from: https://jrsoftware.org/isinfo.php"
    exit 1
}

Write-Host "Building HomeFaceLogon installer using Inno Setup..."
& $iscc installer\HomeFaceLogon.iss
if ($LASTEXITCODE -eq 0) {
    Write-Host "Installer compiled successfully! Output is in installer_output\"
} else {
    Write-Error "Failed to compile installer."
    exit $LASTEXITCODE
}
