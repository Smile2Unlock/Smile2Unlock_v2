#pragma once

#include "Smile2Unlock/backend/models/types.h"
#include <string>
#include <memory>

namespace smile2unlock {
namespace managers {

class FaceRecognition {
public:
    FaceRecognition();
    ~FaceRecognition();

    bool Initialize(std::string& error_message);
    bool IsInitialized() const { return initialized_; }
    
    bool StartRecognition();
    void StopRecognition();
    bool IsRunning() const { return is_running_; }
    
    RecognitionResult GetLastResult() const;
    std::string ExtractFaceFeature(const std::string& image_path);
    bool CompareFaces(const std::string& feature1, const std::string& feature2, float& similarity);

private:
    bool initialized_;
    bool is_running_;
    RecognitionResult last_result_;
    
    // TODO: SeetaFace6 相关成员变量
    // std::unique_ptr<seeta::FaceDetector> face_detector_;
    // std::unique_ptr<seeta::FaceLandmarker> face_landmarker_;
    // std::unique_ptr<seeta::FaceRecognizer> face_recognizer_;
};

} // namespace managers
} // namespace smile2unlock
