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

bool BackendService::AddUser(const std::string& username, const std::string& password, std::string& error_message) {
    if (username.empty()) {
        error_message = "Username cannot be empty";
        return false;
    }

    if (database_->UserExists(username)) {
        error_message = "Username already exists";
        return false;
    }

    User new_user;
    new_user.id = 0;
    new_user.username = username;
    new_user.encrypted_password = password;
    new_user.face_feature = "TODO";

    if (database_->AddUser(new_user)) {
        error_message = "User added successfully";
        return true;
    } else {
        error_message = "Failed to add user";
        return false;
    }
}

bool BackendService::DeleteUser(int userId, std::string& error_message) {
    if (database_->DeleteUser(userId)) {
        error_message = "User deleted successfully";
        return true;
    } else {
        error_message = "Failed to delete user";
        return false;
    }
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
