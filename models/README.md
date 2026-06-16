# Face Recognition Models

This directory contains the ONNX face recognition models downloaded by `tools/fetch-models.ps1`.

## Models Used

### 1. YuNet (Face Detection)
- **File**: `face_detection_yunet_2023mar.onnx`
- **Purpose**: Fast, lightweight face detection optimized for CPU.
- **License**: Apache 2.0 (OpenCV Zoo)
- **Source**: [OpenCV face_detection_yunet on Hugging Face](https://huggingface.co/opencv/face_detection_yunet)

### 2. SFace (Face Recognition)
- **File**: `face_recognition_sface_2021dec.onnx`
- **Purpose**: Cosine similarity based face recognition/feature extraction.
- **License**: Apache 2.0 (OpenCV Zoo)
- **Source**: [OpenCV face_recognition_sface on Hugging Face](https://huggingface.co/opencv/face_recognition_sface)

## Download

Run the following command to download the models locally:
```powershell
powershell -ExecutionPolicy Bypass -File .\tools\fetch-models.ps1
```
