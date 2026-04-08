#pragma once

#include "../models.h"
#include <string>
#include <vector>

namespace smile2unlock {

/**
 * @brief 后端服务接口
 * 
 * 定义了 GUI 和 Service 之间的通信接口
 */
class IBackendService {
public:
    virtual ~IBackendService() = default;

    virtual bool Initialize() = 0;
    virtual bool IsInitialized() const = 0;

    // DLL 管理
    virtual DllStatus GetDllStatus() = 0;
    virtual bool CalculateDllHashes() = 0;
    virtual bool InjectDll(std::string& error_message) = 0;

    // 用户管理
    virtual std::vector<User> GetAllUsers() = 0;
    virtual bool AddUser(const std::string& username, const std::string& password, const std::string& remark, std::string& error_message) = 0;
    virtual bool UpdateUser(int user_id, const std::string& username, const std::string& password, const std::string& remark, std::string& error_message) = 0;
    virtual bool DeleteUser(int userId, std::string& error_message) = 0;

    // 人脸管理
    virtual bool AddFace(int user_id, const std::string& feature, const std::string& remark, std::string& error_message) = 0;
    virtual bool StartCameraPreview(std::string& error_message) = 0;
    virtual void StopCameraPreview() = 0;
    virtual bool IsCameraPreviewRunning() const = 0;
    virtual bool GetLatestCameraPreview(std::vector<unsigned char>& image_data, int& width, int& height, std::string& error_message) = 0;
    virtual bool CapturePreviewFrame(std::vector<unsigned char>& image_data, int& width, int& height, std::string& error_message) = 0;
    virtual bool CaptureAndAddFace(int user_id, const std::string& remark, std::string& error_message) = 0;
    virtual bool DeleteFace(int user_id, int face_id, std::string& error_message) = 0;
    virtual bool UpdateFaceRemark(int user_id, int face_id, const std::string& remark, std::string& error_message) = 0;
    virtual std::vector<FaceData> GetUserFaces(int user_id) = 0;

    // 人脸识别
    virtual bool StartRecognition(std::string& error_message) = 0;
    virtual void StopRecognition() = 0;
    virtual bool IsRecognitionRunning() const = 0;
    virtual RecognitionResult GetRecognitionResult() = 0;
    virtual bool GetRecognizerConfig(FaceRecognizerConfig& config, std::string& error_message) = 0;
    virtual bool SaveRecognizerConfig(const FaceRecognizerConfig& config, std::string& error_message) = 0;
};

} // namespace smile2unlock
