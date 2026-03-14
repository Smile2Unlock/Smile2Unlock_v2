#include "Smile2Unlock/backend/service.h"

namespace smile2unlock {

BackendService::BackendService() : initialized_(false) {
    dll_injector_ = std::make_unique<managers::DllInjector>();
    database_ = std::make_unique<managers::Database>();
    face_recognition_ = std::make_unique<managers::FaceRecognition>();
}

BackendService::~BackendService() = default;

bool BackendService::Initialize() {
    if (initialized_) {
        return true;
    }

    if (!database_->Initialize()) {
        return false;
    }

    initialized_ = true;
    return true;
}

DllStatus BackendService::GetDllStatus() {
    return dll_injector_->GetStatus();
}

bool BackendService::CalculateDllHashes() {
    return true;
}

bool BackendService::InjectDll(std::string& error_message) {
    return dll_injector_->InjectDll(error_message);
}

std::vector<User> BackendService::GetAllUsers() {
    return database_->GetAllUsers();
}

bool BackendService::AddUser(const std::string& username, const std::string& password, const std::string& remark, std::string& error_message) {
    if (username.empty()) {
        error_message = "用户名不能为空";
        return false;
    }

    if (database_->UserExists(username)) {
        error_message = "用户名已存在";
        return false;
    }

    User new_user;
    new_user.id = 0;
    new_user.username = username;
    new_user.encrypted_password = password; // TODO: 加密
    new_user.remark = remark;

    if (database_->AddUser(new_user)) {
        error_message = "用户添加成功";
        return true;
    } else {
        error_message = "添加用户失败";
        return false;
    }
}

bool BackendService::UpdateUser(int user_id, const std::string& username, const std::string& password, const std::string& remark, std::string& error_message) {
    auto user = database_->GetUserById(user_id);
    if (!user.has_value()) {
        error_message = "用户不存在";
        return false;
    }

    User updated_user = user.value();
    updated_user.username = username;
    if (!password.empty()) {
        updated_user.encrypted_password = password; // TODO: 加密
    }
    updated_user.remark = remark;

    if (database_->UpdateUser(updated_user)) {
        error_message = "用户更新成功";
        return true;
    } else {
        error_message = "更新用户失败";
        return false;
    }
}

bool BackendService::DeleteUser(int userId, std::string& error_message) {
    if (database_->DeleteUser(userId)) {
        error_message = "用户删除成功";
        return true;
    } else {
        error_message = "删除用户失败";
        return false;
    }
}

bool BackendService::AddFace(int user_id, const std::string& image_path, const std::string& remark, std::string& error_message) {
    // 提取人脸特征
    std::string feature = face_recognition_->ExtractFaceFeature(image_path);
    if (feature.empty()) {
        error_message = "人脸特征提取失败";
        return false;
    }

    FaceData face;
    face.feature = feature;
    face.image_path = image_path;
    face.remark = remark;

    if (database_->AddFace(user_id, face)) {
        error_message = "人脸添加成功";
        return true;
    } else {
        error_message = "添加人脸失败";
        return false;
    }
}

bool BackendService::DeleteFace(int user_id, int face_id, std::string& error_message) {
    if (database_->DeleteFace(user_id, face_id)) {
        error_message = "人脸删除成功";
        return true;
    } else {
        error_message = "删除人脸失败";
        return false;
    }
}

bool BackendService::UpdateFaceRemark(int user_id, int face_id, const std::string& remark, std::string& error_message) {
    FaceData face;
    face.id = face_id;
    face.remark = remark;
    
    if (database_->UpdateFace(user_id, face)) {
        error_message = "备注更新成功";
        return true;
    } else {
        error_message = "更新备注失败";
        return false;
    }
}

std::vector<FaceData> BackendService::GetUserFaces(int user_id) {
    return database_->GetUserFaces(user_id);
}

bool BackendService::StartRecognition(std::string& error_message) {
    return face_recognition_->Initialize(error_message) && face_recognition_->StartRecognition();
}

void BackendService::StopRecognition() {
    face_recognition_->StopRecognition();
}

bool BackendService::IsRecognitionRunning() const {
    return face_recognition_->IsRunning();
}

RecognitionResult BackendService::GetRecognitionResult() {
    return face_recognition_->GetLastResult();
}

} // namespace smile2unlock
