#include "DllLoader.h"
#include <opencv2/opencv.hpp>
import std;


namespace seeta
{
    namespace cv
    {
        // using namespace ::cv;
        class ImageData : public SeetaImageData {
        public:
            ImageData(const ::cv::Mat& mat)
                : cv_mat(mat.clone()) {
                this->width = cv_mat.cols;
                this->height = cv_mat.rows;
                this->channels = cv_mat.channels();
                this->data = cv_mat.data;
            }
        private:
            ::cv::Mat cv_mat;
        };
    }
}

Dllloader::Dllloader()
{
    capThread = nullptr;
    recognizer = nullptr;
    currentSimilarity = 0.0f;
    recognitionStatus = 0; // 未开始
    cameraInitialized = false;
}

Dllloader::~Dllloader()
{
    Cleanup();
}

bool Dllloader::InitializeCamera()
{
    if (cameraInitialized) {
        return true;
    }
    
    capThread = new CapThread();
    
    // 等待摄像头线程填充首帧（最多等待 5s）
    const int max_wait_ms = 5000;
    int waited = 0;
    while (waited < max_wait_ms) {
        currentFrame = capThread->getImage();
        if (!currentFrame.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        waited += 50;
    }

    if (currentFrame.empty()) {
        cameraInitialized = false;
        return false;
    }
    
    cameraInitialized = true;
    return true;
}

bool Dllloader::GetCameraFrame()
{
    if (!cameraInitialized) {
        return false;
    }
    
    currentFrame = capThread->getImage();
    return !currentFrame.empty();
}

bool Dllloader::FaceEnrollment(const std::string& dataFile)
{
    if (!cameraInitialized) {
        if (!InitializeCamera()) {
            return false;
        }
    }
    
    if (!GetCameraFrame()) {
        return false;
    }
    
    recognizer = new Recognizer();
    SeetaImageData image_data_frame = seeta::cv::ImageData(currentFrame);
    auto feat = recognizer->tofeats(image_data_frame);
    recognizer->savefeat(feat, dataFile);
    
    recognitionStatus = 2; // 录入成功
    return true;
}

bool Dllloader::FaceRecognition(const std::string& dataFile)
{
    if (!cameraInitialized) {
        if (!InitializeCamera()) {
            return false;
        }
    }
    
    recognizer = new Recognizer();
    auto feat1 = recognizer->loadfeat(dataFile);
    
    if (!feat1) {
        recognitionStatus = 3; // 识别失败（无法加载特征数据）
        return false;
    }
    
    std::shared_ptr<float> feat2;
    int time_count = 0;
    int anti_count = 0;
    
    recognitionStatus = 1; // 识别中
    
    while (true)
    {
        if (!GetCameraFrame()) {
            recognitionStatus = 3; // 识别失败（无法获取帧）
            return false;
        }
        
        SeetaImageData image_data_frame = seeta::cv::ImageData(currentFrame);
        
        // 活体检测
        auto anti = recognizer->anti_face(image_data_frame);
        if (!anti)
        {
            anti_count++;
        }
        if (anti_count > 50)
        {
            recognitionStatus = 4; // 活体检测失败
            return false;
        }

        // 人脸识别
        feat2 = recognizer->tofeats(image_data_frame);
        currentSimilarity = recognizer->feat_compare(feat1, feat2);
        
        if (currentSimilarity > 0.62f && anti)
        {
            recognitionStatus = 2; // 识别成功
            return true;
        }
        
        if (currentSimilarity < 0.62f && time_count > 10)
        {
            recognitionStatus = 3; // 识别失败
            return false;
        }
        
        time_count++;
        
        // 添加短暂延迟，避免过度占用CPU
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

float Dllloader::GetSimilarity()
{
    return currentSimilarity;
}

int Dllloader::GetRecognitionStatus()
{
    return recognitionStatus;
}

void Dllloader::Cleanup()
{
    if (capThread) {
        delete capThread;
        capThread = nullptr;
    }
    
    if (recognizer) {
        delete recognizer;
        recognizer = nullptr;
    }
    
    cameraInitialized = false;
    recognitionStatus = 0;
    currentSimilarity = 0.0f;
}

// C风格导出函数实现
static Dllloader* g_dllLoader = nullptr;

extern "C" FRLIBRARY_API bool FR_InitializeCamera()
{
    if (!g_dllLoader) {
        g_dllLoader = new Dllloader();
    }
    return g_dllLoader->InitializeCamera();
}

extern "C" FRLIBRARY_API bool FR_FaceEnrollment(const char* dataFile)
{
    if (!g_dllLoader) {
        g_dllLoader = new Dllloader();
    }
    return g_dllLoader->FaceEnrollment(std::string(dataFile));
}

extern "C" FRLIBRARY_API bool FR_FaceRecognition(const char* dataFile)
{
    if (!g_dllLoader) {
        g_dllLoader = new Dllloader();
    }
    return g_dllLoader->FaceRecognition(std::string(dataFile));
}

extern "C" FRLIBRARY_API float FR_GetSimilarity()
{
    if (!g_dllLoader) {
        return 0.0f;
    }
    return g_dllLoader->GetSimilarity();
}

extern "C" FRLIBRARY_API int FR_GetRecognitionStatus()
{
    if (!g_dllLoader) {
        return 0;
    }
    return g_dllLoader->GetRecognitionStatus();
}

extern "C" FRLIBRARY_API void FR_Cleanup()
{
    if (g_dllLoader) {
        g_dllLoader->Cleanup();
        delete g_dllLoader;
        g_dllLoader = nullptr;
    }
}