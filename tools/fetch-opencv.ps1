# tools/fetch-opencv.ps1
$ErrorActionPreference = 'Stop'

$workDir = Resolve-Path "$PSScriptRoot\.."
$targetDir = Join-Path $workDir "third_party"
$opencvExe = Join-Path $workDir "opencv-4.10.0.exe"

if (-not (Test-Path $targetDir)) {
    New-Item -ItemType Directory -Path $targetDir -Force | Out-Null
}

Write-Host "Downloading OpenCV 4.10.0 Windows package..." -ForegroundColor Cyan
# Using curl.exe for faster download and redirects handling
& curl.exe -L -o $opencvExe "https://github.com/opencv/opencv/releases/download/4.10.0/opencv-4.10.0-windows.exe"

if (-not (Test-Path $opencvExe)) {
    Write-Error "Failed to download OpenCV package."
    exit 1
}

Write-Host "Extracting OpenCV..." -ForegroundColor Cyan
# Run the self-extracting zip silently
$extractProcess = Start-Process -FilePath $opencvExe -ArgumentList "-y", "-o`"$targetDir`"" -Wait -NoNewWindow -PassThru

if ($extractProcess.ExitCode -ne 0) {
    Write-Error "OpenCV extraction failed with exit code $($extractProcess.ExitCode)."
    Remove-Item $opencvExe -Force -ErrorAction SilentlyContinue
    exit 1
}

# Clean up self-extracting EXE
Remove-Item $opencvExe -Force -ErrorAction SilentlyContinue

Write-Host "OpenCV successfully downloaded and extracted to: $($targetDir)\opencv" -ForegroundColor Green
