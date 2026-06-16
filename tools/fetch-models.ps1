# tools/fetch-models.ps1
$ErrorActionPreference = 'Stop'

$workDir = Resolve-Path "$PSScriptRoot\.."
$modelsDir = Join-Path $workDir "models"

if (-not (Test-Path $modelsDir)) {
    New-Item -ItemType Directory -Path $modelsDir -Force | Out-Null
}

$models = @{
    "face_detection_yunet_2023mar.onnx" = "https://huggingface.co/opencv/face_detection_yunet/resolve/main/face_detection_yunet_2023mar.onnx"
    "face_recognition_sface_2021dec.onnx" = "https://huggingface.co/opencv/face_recognition_sface/resolve/main/face_recognition_sface_2021dec.onnx"
}

foreach ($item in $models.GetEnumerator()) {
    $name = $item.Key
    $url = $item.Value
    $targetPath = Join-Path $modelsDir $name

    Write-Host "Downloading model: $name..." -ForegroundColor Cyan
    & curl.exe -L -o $targetPath $url

    if (-not (Test-Path $targetPath)) {
        Write-Error "Failed to download model $name."
        exit 1
    }
}

Write-Host "All models downloaded successfully to: $modelsDir" -ForegroundColor Green
