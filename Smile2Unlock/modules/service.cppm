module;

#include "models/gui_ipc_protocol.h"
#include "backend/ibackend_service.h"

export module smile2unlock.service;

import std;
import app_paths;
import config;
import smile2unlock.models;
import smile2unlock.database;
import smile2unlock.dll_injector;
import smile2unlock.face_recognition;

// 重新导出 IBackendService 接口（定义在 backend/ibackend_service.h）
export using ::smile2unlock::IBackendService;

export namespace smile2unlock {

class BackendService : public IBackendService {
public:
    BackendService();
    ~BackendService() = default;

    bool Initialize();
    bool IsInitialized() const { return initialized_; }

    DllStatus GetDllStatus();
    bool CalculateDllHashes();
    bool InjectDll(std::string& error_message);

    std::vector<User> GetAllUsers();
    bool AddUser(const std::string& username, const std::string& password, const std::string& remark, std::string& error_message);
    bool UpdateUser(int user_id, const std::string& username, const std::string& password, const std::string& remark, std::string& error_message);
    bool DeleteUser(int userId, std::string& error_message);

    bool AddFace(int user_id, const std::string& feature, const std::string& remark, std::string& error_message);
    bool StartCameraPreview(std::string& error_message);
    void StopCameraPreview();
    bool IsCameraPreviewRunning() const;
    bool GetLatestCameraPreview(std::vector<unsigned char>& image_data, int& width, int& height, std::string& error_message);
    bool CapturePreviewFrame(std::vector<unsigned char>& image_data, int& width, int& height, std::string& error_message);
    bool CaptureAndAddFace(int user_id, const std::string& remark, std::string& error_message);
    bool DeleteFace(int user_id, int face_id, std::string& error_message);
    bool UpdateFaceRemark(int user_id, int face_id, const std::string& remark, std::string& error_message);
    std::vector<FaceData> GetUserFaces(int user_id);

    bool StartRecognition(std::string& error_message);
    void StopRecognition();
    bool IsRecognitionRunning() const;
    RecognitionResult GetRecognitionResult();
    bool GetRecognizerConfig(FaceRecognizerConfig& config, std::string& error_message);
    bool SaveRecognizerConfig(const FaceRecognizerConfig& config, std::string& error_message);

private:
    bool ApplyRecognizerConfigFromStorage(std::string& error_message);
    bool initialized_;
    std::unique_ptr<managers::DllInjector> dll_injector_;
    std::unique_ptr<managers::Database> database_;
    std::unique_ptr<managers::FaceRecognition> face_recognition_;
    std::unique_ptr<ConfigManager> config_manager_;
};

} // namespace smile2unlock

module :private;

namespace smile2unlock {

namespace {

std::string EncryptPasswordForPersistence(const std::string& password, std::string& error_message) {
    if (password.empty()) {
        error_message = "密码不能为空";
        return {};
    }

    const std::string encrypted_password = managers::EncryptPasswordForStorage(
        password, smile2unlock::paths::GetDatabasePath().string());
    if (encrypted_password.empty()) {
        error_message = "密码加密失败";
        return {};
    }

    error_message.clear();
    return encrypted_password;
}

}

BackendService::BackendService() : initialized_(false) {
    dll_injector_ = std::make_unique<managers::DllInjector>();
    database_ = std::make_unique<managers::Database>();
    face_recognition_ = std::make_unique<managers::FaceRecognition>();
    config_manager_ = std::make_unique<ConfigManager>(smile2unlock::paths::GetConfigIniPath().string());
}

bool BackendService::Initialize() {
    if (initialized_) return true;
    if (!database_->Initialize()) return false;
    std::string config_error;
    if (!ApplyRecognizerConfigFromStorage(config_error)) {
        return false;
    }

    std::string error_message;
    if (!face_recognition_->Initialize(error_message)) return false;
    initialized_ = true;
    return true;
}

DllStatus BackendService::GetDllStatus() { return dll_injector_->GetStatus(); }
bool BackendService::CalculateDllHashes() { return true; }
bool BackendService::InjectDll(std::string& error_message) { return dll_injector_->InjectDll(error_message); }
std::vector<User> BackendService::GetAllUsers() { return database_->GetAllUsers(); }

bool BackendService::AddUser(const std::string& username, const std::string& password, const std::string& remark, std::string& error_message) {
    if (username.empty()) {
        error_message = "用户名不能为空";
        return false;
    }
    if (password.empty()) {
        error_message = "密码不能为空";
        return false;
    }
    if (database_->UserExists(username)) {
        error_message = "用户名已存在";
        return false;
    }
    User new_user;
    new_user.username = username;
    new_user.encrypted_password = EncryptPasswordForPersistence(password, error_message);
    if (new_user.encrypted_password.empty()) {
        return false;
    }
    new_user.remark = remark;
    if (database_->AddUser(new_user)) {
        error_message = "用户添加成功";
        return true;
    }
    error_message = "添加用户失败";
    return false;
}

bool BackendService::UpdateUser(int user_id, const std::string& username, const std::string& password, const std::string& remark, std::string& error_message) {
    auto user = database_->GetUserById(user_id);
    if (!user.has_value()) {
        error_message = "用户不存在";
        return false;
    }
    if (username.empty()) {
        error_message = "用户名不能为空";
        return false;
    }
    auto same_name_user = database_->GetUserByUsername(username);
    if (same_name_user.has_value() && same_name_user->id != user_id) {
        error_message = "用户名已存在";
        return false;
    }
    User updated_user = user.value();
    updated_user.username = username;
    if (!password.empty()) {
        updated_user.encrypted_password = EncryptPasswordForPersistence(password, error_message);
        if (updated_user.encrypted_password.empty()) {
            return false;
        }
    }
    updated_user.remark = remark;
    if (database_->UpdateUser(updated_user)) {
        error_message = "用户更新成功";
        return true;
    }
    error_message = "更新用户失败";
    return false;
}

bool BackendService::DeleteUser(int userId, std::string& error_message) {
    if (database_->DeleteUser(userId)) {
        error_message = "用户删除成功";
        return true;
    }
    error_message = "删除用户失败";
    return false;
}

bool BackendService::AddFace(int user_id, const std::string& feature, const std::string& remark, std::string& error_message) {
    if (feature.empty()) {
        error_message = "人脸特征不能为空";
        return false;
    }

    FaceData face;
    face.feature = feature;
    face.image_path.clear();
    face.remark = remark;
    if (database_->AddFace(user_id, face)) {
        error_message = "人脸添加成功";
        return true;
    }
    error_message = "添加人脸失败";
    return false;
}

bool BackendService::StartCameraPreview(std::string& error_message) {
    if (!ApplyRecognizerConfigFromStorage(error_message)) {
        return false;
    }
    return face_recognition_->StartPreviewStream(error_message);
}

void BackendService::StopCameraPreview() {
    face_recognition_->StopPreviewStream();
}

bool BackendService::IsCameraPreviewRunning() const {
    return face_recognition_->IsPreviewStreamRunning();
}

bool BackendService::GetLatestCameraPreview(std::vector<unsigned char>& image_data, int& width, int& height, std::string& error_message) {
    return face_recognition_->GetLatestPreviewFrame(image_data, width, height, error_message);
}

bool BackendService::CapturePreviewFrame(std::vector<unsigned char>& image_data, int& width, int& height, std::string& error_message) {
    return face_recognition_->CaptureFrameImage(image_data, width, height, error_message);
}

bool BackendService::CaptureAndAddFace(int user_id, const std::string& remark, std::string& error_message) {
    auto user = database_->GetUserById(user_id);
    if (!user.has_value()) {
        error_message = "用户不存在";
        return false;
    }

    const bool restart_preview = face_recognition_->IsPreviewStreamRunning();
    if (restart_preview) {
        face_recognition_->StopPreviewStream();
    }

    std::vector<unsigned char> image_data;
    int width = 0;
    int height = 0;
    std::string feature;
    const bool captured = face_recognition_->CaptureFrameAndFeature(image_data, width, height, feature, error_message);

    if (restart_preview) {
        std::string restart_error;
        ApplyRecognizerConfigFromStorage(restart_error);
        face_recognition_->StartPreviewStream(restart_error);
    }

    if (!captured) {
        return false;
    }

    return AddFace(user_id, feature, remark, error_message);
}

bool BackendService::DeleteFace(int user_id, int face_id, std::string& error_message) {
    if (database_->DeleteFace(user_id, face_id)) {
        error_message = "人脸删除成功";
        return true;
    }
    error_message = "删除人脸失败";
    return false;
}

bool BackendService::UpdateFaceRemark(int user_id, int face_id, const std::string& remark, std::string& error_message) {
    FaceData face;
    face.id = face_id;
    face.remark = remark;
    if (database_->UpdateFace(user_id, face)) {
        error_message = "备注更新成功";
        return true;
    }
    error_message = "更新备注失败";
    return false;
}

std::vector<FaceData> BackendService::GetUserFaces(int user_id) { return database_->GetUserFaces(user_id); }

bool BackendService::StartRecognition(std::string& error_message) {
    if (!face_recognition_->IsInitialized() && !face_recognition_->Initialize(error_message)) return false;
    if (!ApplyRecognizerConfigFromStorage(error_message)) {
        return false;
    }
    return face_recognition_->StartRecognition();
}
void BackendService::StopRecognition() { face_recognition_->StopRecognition(); }
bool BackendService::IsRecognitionRunning() const { return face_recognition_->IsRunning(); }
RecognitionResult BackendService::GetRecognitionResult() { return face_recognition_->GetLastResult(); }

bool BackendService::GetRecognizerConfig(FaceRecognizerConfig& config, std::string& error_message) {
    if (!config_manager_->loadConfig()) {
        if (!config_manager_->createDefaultConfig() || !config_manager_->loadConfig()) {
            error_message = "加载识别配置失败";
            return false;
        }
    }

    const auto& loaded = config_manager_->getConfig();
    config.camera = loaded.camera;
    config.liveness = loaded.liveness;
    config.face_threshold = loaded.face_threshold;
    config.liveness_threshold = loaded.liveness_threshold;
    config.debug = loaded.debug;
    face_recognition_->SetConfig(config);
    error_message = "识别配置已加载";
    return true;
}

bool BackendService::SaveRecognizerConfig(const FaceRecognizerConfig& config, std::string& error_message) {
    ConfigManager::CoreConfig core_config;
    core_config.camera = config.camera;
    core_config.liveness = config.liveness;
    core_config.face_threshold = config.face_threshold;
    core_config.liveness_threshold = config.liveness_threshold;
    core_config.debug = config.debug;

    config_manager_->setConfig(core_config);
    if (!config_manager_->saveConfig()) {
        error_message = "保存识别配置失败";
        return false;
    }

    face_recognition_->SetConfig(config);
    error_message = "识别配置已保存";
    return true;
}

bool BackendService::ApplyRecognizerConfigFromStorage(std::string& error_message) {
    FaceRecognizerConfig config;
    if (!GetRecognizerConfig(config, error_message)) {
        return false;
    }

    face_recognition_->SetConfig(config);
    error_message.clear();
    return true;
}

} // namespace smile2unlock
