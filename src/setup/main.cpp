#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <iostream>
#include <string>
#include <fstream>
#include <vector>
#include <filesystem>
#include <memory>
#include <cmath>

// OpenCV headers
#include <opencv2/opencv.hpp>
#include <opencv2/objdetect.hpp>
#include <opencv2/dnn.hpp>

#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "Shlwapi.lib")

namespace fs = std::filesystem;

struct SecretFileHeader {
    uint32_t magic;          // 'HFLO' (0x4F4C4648)
    uint16_t schemaVersion;  // 1
    uint16_t flags;          // 0
    uint32_t protectedBlobSize;
};

struct FaceFileHeader {
    uint32_t magic;          // 'HFLF' (0x464C4648)
    uint16_t schemaVersion;  // 1
    uint16_t flags;          // 0
    uint32_t protectedBlobSize;
};

const uint32_t HFLO_MAGIC = 0x4F4C4648;
const uint32_t HFLF_MAGIC = 0x464C4648;

std::string utf16_to_utf8(const std::wstring& wstr)
{
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

std::wstring GetModelPath(const std::wstring& filename) {
    wchar_t programData[MAX_PATH];
    
    // 1. Check %ProgramData%\HomeFaceLogon\models\<filename>
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, programData))) {
        fs::path p1 = fs::path(programData) / L"HomeFaceLogon" / L"models" / filename;
        if (fs::exists(p1)) return p1.wstring();
        
        fs::path p2 = fs::path(programData) / L"HomeFaceLogon" / filename;
        if (fs::exists(p2)) return p2.wstring();
    }

    // 2. Check relative to executable path L"models/" + filename
    wchar_t exePath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH)) {
        fs::path exeDir = fs::path(exePath).parent_path();
        fs::path p3 = exeDir / L"models" / filename;
        if (fs::exists(p3)) return p3.wstring();
        
        fs::path p4 = exeDir / filename;
        if (fs::exists(p4)) return p4.wstring();
    }

    // 3. Check relative to current working directory
    fs::path p5 = fs::path(L"models") / filename;
    if (fs::exists(p5)) return p5.wstring();

    fs::path p6 = filename;
    if (fs::exists(p6)) return p6.wstring();

    // Fallback path in %ProgramData%
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, programData))) {
        return (fs::path(programData) / L"HomeFaceLogon" / L"models" / filename).wstring();
    }
    return filename;
}

bool SaveSecret(const std::wstring& password)
{
    DATA_BLOB input;
    input.pbData = reinterpret_cast<BYTE*>(const_cast<wchar_t*>(password.c_str()));
    input.cbData = static_cast<DWORD>((password.length() + 1) * sizeof(wchar_t));

    DATA_BLOB output = {0};
    
    if (!CryptProtectData(
        &input,
        L"HomeFaceLogon MSA password",
        nullptr,
        nullptr,
        nullptr,
        CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN,
        &output
    ))
    {
        std::wcerr << L"CryptProtectData failed: " << GetLastError() << std::endl;
        return false;
    }

    wchar_t path[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, path)))
    {
        LocalFree(output.pbData);
        return false;
    }
    PathAppendW(path, L"HomeFaceLogon");
    CreateDirectoryW(path, nullptr);
    PathAppendW(path, L"secret.bin");

    std::ofstream file(path, std::ios::out | std::ios::binary);
    if (!file.is_open())
    {
        std::wcerr << L"Failed to open secret.bin for writing." << std::endl;
        LocalFree(output.pbData);
        return false;
    }

    SecretFileHeader header;
    header.magic = HFLO_MAGIC;
    header.schemaVersion = 1;
    header.flags = 0;
    header.protectedBlobSize = output.cbData;

    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(output.pbData), output.cbData);
    file.close();

    LocalFree(output.pbData);
    return true;
}

bool SaveConfig(const std::wstring& sid)
{
    wchar_t path[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, path)))
    {
        return false;
    }
    PathAppendW(path, L"HomeFaceLogon");
    CreateDirectoryW(path, nullptr);
    PathAppendW(path, L"config.json");

    std::ofstream file(path, std::ios::out | std::ios::binary);
    if (!file.is_open())
    {
        std::wcerr << L"Failed to open config.json for writing." << std::endl;
        return false;
    }

    int len = WideCharToMultiByte(CP_UTF8, 0, sid.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8Sid(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, sid.c_str(), -1, &utf8Sid[0], len, nullptr, nullptr);

    file << "{\n";
    file << "  \"schemaVersion\": 1,\n";
    file << "  \"enabled\": true,\n";
    file << "  \"targetSid\": \"" << utf8Sid << "\"\n";
    file << "}\n";
    file.close();

    return true;
}

bool SaveFaceTemplate(const std::vector<float>& featureVec)
{
    DATA_BLOB input;
    input.pbData = reinterpret_cast<BYTE*>(const_cast<float*>(featureVec.data()));
    input.cbData = static_cast<DWORD>(featureVec.size() * sizeof(float));

    DATA_BLOB output = {0};
    
    if (!CryptProtectData(
        &input,
        L"HomeFaceLogon Face Template",
        nullptr,
        nullptr,
        nullptr,
        CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN,
        &output
    ))
    {
        std::wcerr << L"CryptProtectData failed: " << GetLastError() << std::endl;
        return false;
    }

    wchar_t path[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, path)))
    {
        LocalFree(output.pbData);
        return false;
    }
    PathAppendW(path, L"HomeFaceLogon");
    CreateDirectoryW(path, nullptr);
    PathAppendW(path, L"face.bin");

    std::ofstream file(path, std::ios::out | std::ios::binary);
    if (!file.is_open())
    {
        std::wcerr << L"Failed to open face.bin for writing." << std::endl;
        LocalFree(output.pbData);
        return false;
    }

    FaceFileHeader header;
    header.magic = HFLF_MAGIC;
    header.schemaVersion = 1;
    header.flags = 0;
    header.protectedBlobSize = output.cbData;

    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(output.pbData), output.cbData);
    file.close();

    LocalFree(output.pbData);
    return true;
}

bool LoadFaceTemplate(std::vector<float>& featureVec)
{
    wchar_t path[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, path)))
    {
        return false;
    }
    PathAppendW(path, L"HomeFaceLogon");
    PathAppendW(path, L"face.bin");

    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file.is_open())
    {
        std::wcerr << L"Failed to open face.bin for reading. Make sure to --enroll first." << std::endl;
        return false;
    }

    FaceFileHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (header.magic != HFLF_MAGIC || header.schemaVersion != 1)
    {
        std::wcerr << L"Invalid face.bin header." << std::endl;
        return false;
    }

    std::vector<BYTE> encryptedBlob(header.protectedBlobSize);
    file.read(reinterpret_cast<char*>(encryptedBlob.data()), header.protectedBlobSize);
    file.close();

    DATA_BLOB input;
    input.pbData = encryptedBlob.data();
    input.cbData = header.protectedBlobSize;

    DATA_BLOB output = {0};

    if (!CryptUnprotectData(
        &input,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        CRYPTPROTECT_UI_FORBIDDEN,
        &output
    ))
    {
        std::wcerr << L"CryptUnprotectData failed: " << GetLastError() << std::endl;
        return false;
    }

    if (output.cbData != 128 * sizeof(float))
    {
        std::wcerr << L"Decrypted face template size mismatch: " << output.cbData << L" bytes (expected 512)" << std::endl;
        LocalFree(output.pbData);
        return false;
    }

    featureVec.resize(128);
    memcpy(featureVec.data(), output.pbData, 128 * sizeof(float));
    LocalFree(output.pbData);
    return true;
}

bool EnrollFace(int cameraIndex)
{
    std::wstring wModelDet = GetModelPath(L"face_detection_yunet_2023mar.onnx");
    std::wstring wModelRec = GetModelPath(L"face_recognition_sface_2021dec.onnx");

    if (!fs::exists(wModelDet) || !fs::exists(wModelRec))
    {
        std::wcerr << L"Error: Required ONNX models are missing. Make sure to run tools/fetch-models.ps1." << std::endl;
        std::wcerr << L"Expected paths:\n  " << wModelDet << L"\n  " << wModelRec << std::endl;
        return false;
    }

    std::string modelDet = utf16_to_utf8(wModelDet);
    std::string modelRec = utf16_to_utf8(wModelRec);

    cv::VideoCapture cap(cameraIndex, cv::CAP_DSHOW);
    if (!cap.isOpened())
    {
        cap.open(cameraIndex);
        if (!cap.isOpened())
        {
            std::wcerr << L"Error: Failed to open camera index " << cameraIndex << std::endl;
            return false;
        }
    }

    int width = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
    int height = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
    if (width <= 0 || height <= 0)
    {
        width = 640;
        height = 480;
    }

    cv::Ptr<cv::FaceDetectorYN> detector;
    try {
        detector = cv::FaceDetectorYN::create(modelDet, "", cv::Size(width, height), 0.9f, 0.3f, 5000);
    } catch (const cv::Exception& e) {
        std::cerr << "Error creating FaceDetectorYN: " << e.what() << std::endl;
        return false;
    }

    cv::Ptr<cv::FaceRecognizerSF> recognizer;
    try {
        recognizer = cv::FaceRecognizerSF::create(modelRec, "");
    } catch (const cv::Exception& e) {
        std::cerr << "Error creating FaceRecognizerSF: " << e.what() << std::endl;
        return false;
    }

    std::cout << "Starting Face Enrollment. Please look at the camera." << std::endl;
    std::cout << "Collecting 30 face samples. Press ESC to cancel." << std::endl;

    std::vector<cv::Mat> collectedFeatures;
    cv::Mat frame;
    const int REQUIRED_SAMPLES = 30;

    while (collectedFeatures.size() < REQUIRED_SAMPLES)
    {
        cap >> frame;
        if (frame.empty())
        {
            std::wcerr << L"Failed to grab frame." << std::endl;
            break;
        }

        cv::Mat faces;
        detector->setInputSize(frame.size());
        detector->detect(frame, faces);

        std::string msg = "Enrolling: " + std::to_string(collectedFeatures.size()) + "/" + std::to_string(REQUIRED_SAMPLES);
        cv::Scalar boxColor = cv::Scalar(0, 0, 255); // Red by default

        if (faces.rows == 1)
        {
            cv::Mat aligned;
            recognizer->alignCrop(frame, faces.row(0), aligned);
            cv::Mat feature;
            recognizer->feature(aligned, feature);
            collectedFeatures.push_back(feature.clone());
            
            boxColor = cv::Scalar(0, 255, 0); // Green when exactly one face detected
        }
        else if (faces.rows > 1)
        {
            msg = "Keep only ONE face in frame";
        }
        else
        {
            msg = "No face detected";
        }

        // Draw bounding boxes
        for (int i = 0; i < faces.rows; ++i)
        {
            int x = static_cast<int>(faces.at<float>(i, 0));
            int y = static_cast<int>(faces.at<float>(i, 1));
            int w = static_cast<int>(faces.at<float>(i, 2));
            int h = static_cast<int>(faces.at<float>(i, 3));
            cv::rectangle(frame, cv::Rect(x, y, w, h), boxColor, 2);
        }

        cv::putText(frame, msg, cv::Point(20, 40), cv::FONT_HERSHEY_SIMPLEX, 0.8, boxColor, 2);
        cv::imshow("Face Enrollment - HomeFaceLogon", frame);

        if (cv::waitKey(1) == 27)
        {
            std::cout << "Enrollment cancelled by user." << std::endl;
            cv::destroyAllWindows();
            return false;
        }
    }

    cv::destroyAllWindows();

    if (collectedFeatures.size() < REQUIRED_SAMPLES)
    {
        std::wcerr << L"Failed to collect enough samples." << std::endl;
        return false;
    }

    // Average features
    cv::Mat sumFeature = cv::Mat::zeros(1, 128, CV_32F);
    for (const auto& feat : collectedFeatures)
    {
        sumFeature += feat;
    }
    
    cv::Mat avgFeature;
    cv::normalize(sumFeature, avgFeature, 1, 0, cv::NORM_L2);

    std::vector<float> finalTemplate(128);
    if (avgFeature.isContinuous())
    {
        memcpy(finalTemplate.data(), avgFeature.ptr<float>(), 128 * sizeof(float));
    }
    else
    {
        for (int i = 0; i < 128; ++i)
        {
            finalTemplate[i] = avgFeature.at<float>(0, i);
        }
    }

    if (SaveFaceTemplate(finalTemplate))
    {
        std::cout << "Face template registered and saved successfully." << std::endl;
        return true;
    }
    else
    {
        std::wcerr << L"Failed to save face template." << std::endl;
        return false;
    }
}

bool VerifyFace(int cameraIndex)
{
    std::vector<float> templateVec;
    if (!LoadFaceTemplate(templateVec))
    {
        return false;
    }

    cv::Mat templateFeature = cv::Mat(1, 128, CV_32F, templateVec.data()).clone();

    std::wstring wModelDet = GetModelPath(L"face_detection_yunet_2023mar.onnx");
    std::wstring wModelRec = GetModelPath(L"face_recognition_sface_2021dec.onnx");

    if (!fs::exists(wModelDet) || !fs::exists(wModelRec))
    {
        std::wcerr << L"Error: Required ONNX models are missing. Make sure to run tools/fetch-models.ps1." << std::endl;
        return false;
    }

    std::string modelDet = utf16_to_utf8(wModelDet);
    std::string modelRec = utf16_to_utf8(wModelRec);

    cv::VideoCapture cap(cameraIndex, cv::CAP_DSHOW);
    if (!cap.isOpened())
    {
        cap.open(cameraIndex);
        if (!cap.isOpened())
        {
            std::wcerr << L"Error: Failed to open camera index " << cameraIndex << std::endl;
            return false;
        }
    }

    int width = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
    int height = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
    if (width <= 0 || height <= 0)
    {
        width = 640;
        height = 480;
    }

    cv::Ptr<cv::FaceDetectorYN> detector;
    try {
        detector = cv::FaceDetectorYN::create(modelDet, "", cv::Size(width, height), 0.9f, 0.3f, 5000);
    } catch (const cv::Exception& e) {
        std::cerr << "Error creating FaceDetectorYN: " << e.what() << std::endl;
        return false;
    }

    cv::Ptr<cv::FaceRecognizerSF> recognizer;
    try {
        recognizer = cv::FaceRecognizerSF::create(modelRec, "");
    } catch (const cv::Exception& e) {
        std::cerr << "Error creating FaceRecognizerSF: " << e.what() << std::endl;
        return false;
    }

    std::cout << "Starting Face Verification. Press ESC to stop." << std::endl;

    cv::Mat frame;
    const double MATCH_THRESHOLD = 0.363; // SFace Cosine Similarity threshold

    while (true)
    {
        cap >> frame;
        if (frame.empty())
        {
            std::wcerr << L"Failed to grab frame." << std::endl;
            break;
        }

        cv::Mat faces;
        detector->setInputSize(frame.size());
        detector->detect(frame, faces);

        if (faces.rows > 0)
        {
            for (int i = 0; i < faces.rows; ++i)
            {
                cv::Mat aligned;
                recognizer->alignCrop(frame, faces.row(i), aligned);
                cv::Mat feature;
                recognizer->feature(aligned, feature);

                double score = recognizer->match(feature, templateFeature, cv::FaceRecognizerSF::DisType::FR_COSINE);

                bool isMatch = score >= MATCH_THRESHOLD;
                cv::Scalar boxColor = isMatch ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 0, 255);

                int x = static_cast<int>(faces.at<float>(i, 0));
                int y = static_cast<int>(faces.at<float>(i, 1));
                int w = static_cast<int>(faces.at<float>(i, 2));
                int h = static_cast<int>(faces.at<float>(i, 3));
                cv::rectangle(frame, cv::Rect(x, y, w, h), boxColor, 2);

                std::string label = "Score: " + std::to_string(score).substr(0, 5) + (isMatch ? " (OK)" : " (NG)");
                cv::putText(frame, label, cv::Point(x, y - 10), cv::FONT_HERSHEY_SIMPLEX, 0.6, boxColor, 2);
            }
        }

        cv::imshow("Face Verification - HomeFaceLogon", frame);

        if (cv::waitKey(1) == 27)
        {
            break;
        }
    }

    cv::destroyAllWindows();
    return true;
}

bool RunDiagnostics()
{
    std::cout << "[Diagnostic] Starting system diagnostic checks..." << std::endl;

    // 1. Model file checks
    std::wstring wModelDet = GetModelPath(L"face_detection_yunet_2023mar.onnx");
    std::wstring wModelRec = GetModelPath(L"face_recognition_sface_2021dec.onnx");

    std::wcout << L"[Diagnostic] Detector model path: " << wModelDet << std::endl;
    std::wcout << L"[Diagnostic] Recognizer model path: " << wModelRec << std::endl;

    if (!fs::exists(wModelDet))
    {
        std::wcerr << L"[Diagnostic] ERROR: Detector model file not found." << std::endl;
        return false;
    }
    if (!fs::exists(wModelRec))
    {
        std::wcerr << L"[Diagnostic] ERROR: Recognizer model file not found." << std::endl;
        return false;
    }
    std::cout << "[Diagnostic] OK: Model files exist." << std::endl;

    // 2. Initialize OpenCV models
    std::string modelDet = utf16_to_utf8(wModelDet);
    std::string modelRec = utf16_to_utf8(wModelRec);

    try
    {
        cv::Ptr<cv::FaceDetectorYN> detector = cv::FaceDetectorYN::create(modelDet, "", cv::Size(640, 480), 0.9f, 0.3f, 5000);
        std::cout << "[Diagnostic] OK: FaceDetectorYN initialized successfully." << std::endl;
    }
    catch (const cv::Exception& e)
    {
        std::cerr << "[Diagnostic] ERROR: Failed to initialize FaceDetectorYN: " << e.what() << std::endl;
        return false;
    }

    try
    {
        cv::Ptr<cv::FaceRecognizerSF> recognizer = cv::FaceRecognizerSF::create(modelRec, "");
        std::cout << "[Diagnostic] OK: FaceRecognizerSF initialized successfully." << std::endl;
    }
    catch (const cv::Exception& e)
    {
        std::cerr << "[Diagnostic] ERROR: Failed to initialize FaceRecognizerSF: " << e.what() << std::endl;
        return false;
    }

    // 3. DPAPI encryption/decryption check
    std::cout << "[Diagnostic] Testing DPAPI face template storage..." << std::endl;
    std::vector<float> dummyTemplate(128);
    for (int i = 0; i < 128; ++i)
    {
        dummyTemplate[i] = static_cast<float>(i) / 128.0f;
    }

    // Backup existing face.bin if it exists
    wchar_t path[MAX_PATH];
    bool hasBackup = false;
    std::wstring backupPath;
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, path)))
    {
        fs::path facePath = fs::path(path) / L"HomeFaceLogon" / L"face.bin";
        // Ensure parent directory exists for dummy test
        fs::create_directories(facePath.parent_path());
        if (fs::exists(facePath))
        {
            backupPath = facePath.wstring() + L".bak";
            try {
                fs::copy_file(facePath, backupPath, fs::copy_options::overwrite_existing);
                hasBackup = true;
            } catch(...) {}
        }
    }

    if (!SaveFaceTemplate(dummyTemplate))
    {
        std::cerr << "[Diagnostic] ERROR: SaveFaceTemplate failed." << std::endl;
        return false;
    }
    std::cout << "[Diagnostic] OK: Dummy face template encrypted and saved." << std::endl;

    std::vector<float> loadedTemplate;
    if (!LoadFaceTemplate(loadedTemplate))
    {
        std::cerr << "[Diagnostic] ERROR: LoadFaceTemplate failed." << std::endl;
        return false;
    }
    std::cout << "[Diagnostic] OK: Dummy face template loaded and decrypted." << std::endl;

    if (loadedTemplate.size() != 128)
    {
        std::cerr << "[Diagnostic] ERROR: Decrypted template size is incorrect: " << loadedTemplate.size() << std::endl;
        return false;
    }

    bool match = true;
    for (int i = 0; i < 128; ++i)
    {
        if (std::abs(loadedTemplate[i] - dummyTemplate[i]) > 1e-5)
        {
            match = false;
            break;
        }
    }

    if (!match)
    {
        std::cerr << "[Diagnostic] ERROR: Decrypted template values do not match original." << std::endl;
        return false;
    }
    std::cout << "[Diagnostic] OK: Decrypted template values match original dummy values." << std::endl;

    // Restore backup if existed, otherwise delete dummy face.bin
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, path)))
    {
        fs::path facePath = fs::path(path) / L"HomeFaceLogon" / L"face.bin";
        if (hasBackup)
        {
            try {
                fs::copy_file(backupPath, facePath, fs::copy_options::overwrite_existing);
                fs::remove(backupPath);
                std::cout << "[Diagnostic] Restored original face.bin." << std::endl;
            } catch(...) {}
        }
        else
        {
            try {
                fs::remove(facePath);
                std::cout << "[Diagnostic] Cleaned up temporary face.bin." << std::endl;
            } catch(...) {}
        }
    }

    std::cout << "[Diagnostic] Diagnostic test PASSED." << std::endl;
    return true;
}

int wmain(int argc, wchar_t* argv[])
{
    std::wstring sid;
    std::wstring password;
    bool enroll = false;
    bool verify = false;
    bool test = false;
    int cameraIndex = 0;

    for (int i = 1; i < argc; ++i)
    {
        if (_wcsicmp(argv[i], L"--sid") == 0 && i + 1 < argc)
        {
            sid = argv[++i];
        }
        else if (_wcsicmp(argv[i], L"--password") == 0 && i + 1 < argc)
        {
            password = argv[++i];
        }
        else if (_wcsicmp(argv[i], L"--enroll") == 0)
        {
            enroll = true;
        }
        else if (_wcsicmp(argv[i], L"--verify") == 0)
        {
            verify = true;
        }
        else if (_wcsicmp(argv[i], L"--test") == 0)
        {
            test = true;
        }
        else if (_wcsicmp(argv[i], L"--camera") == 0 && i + 1 < argc)
        {
            cameraIndex = _wtoi(argv[++i]);
        }
    }

    if (test)
    {
        return RunDiagnostics() ? 0 : 1;
    }

    if (enroll)
    {
        return EnrollFace(cameraIndex) ? 0 : 1;
    }

    if (verify)
    {
        return VerifyFace(cameraIndex) ? 0 : 1;
    }

    if (sid.empty())
    {
        std::wcout << L"Enter Target Windows User SID: ";
        std::wcin >> sid;
    }

    if (password.empty())
    {
        std::wcout << L"Enter Microsoft Account Password: ";
        HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode;
        GetConsoleMode(hStdin, &mode);
        SetConsoleMode(hStdin, mode & (~ENABLE_ECHO_INPUT));
        
        std::wcin >> password;
        
        SetConsoleMode(hStdin, mode);
        std::wcout << std::endl;
    }

    if (sid.empty() || password.empty())
    {
        std::wcerr << L"Error: SID and Password cannot be empty." << std::endl;
        return 1;
    }

    if (!SaveSecret(password))
    {
        std::wcerr << L"Failed to save encrypted secret." << std::endl;
        SecureZeroMemory(&password[0], password.length() * sizeof(wchar_t));
        return 1;
    }

    if (!SaveConfig(sid))
    {
        std::wcerr << L"Failed to save configuration." << std::endl;
        SecureZeroMemory(&password[0], password.length() * sizeof(wchar_t));
        return 1;
    }

    SecureZeroMemory(&password[0], password.length() * sizeof(wchar_t));

    std::wcout << L"Setup completed successfully. targetSid and secret.bin updated." << std::endl;
    return 0;
}
