#pragma once

#include "Smile2Unlock/backend/models/types.h"
#include "Smile2Unlock/backend/managers/dll_injector.h"
#include "Smile2Unlock/backend/managers/database.h"
#include "Smile2Unlock/backend/managers/face_recognition.h"
#include <memory>
#include <string>
#include <vector>

namespace smile2unlock {

class BackendService {
public:
    BackendService();
    ~BackendService();

    bool Initialize();
    bool IsInitialized() const { return initialized_; }

    // DLL 注入服务
    DllStatus GetDllStatus();
    bool CalculateDllHashes();
    bool InjectDll(std::string& error_message);

    // 用户管理服务
    std::vector<User> GetAllUsers();
    bool AddUser(const std::string& username, const std::string& password, std::string& error_message);
    bool DeleteUser(int userId, std::string& error_message);

    // 人脸识别服务
    bool StartRecognition(std::string& error_message);
    void StopRecognition();
    bool IsRecognitionRunning() const;
    RecognitionResult GetRecognitionResult();

private:
    bool initialized_;
    
    std::unique_ptr<managers::DllInjector> dll_injector_;
    std::unique_ptr<managers::Database> database_;
    std::unique_ptr<managers::FaceRecognition> face_recognition_;
};

} // namespace smile2unlock
