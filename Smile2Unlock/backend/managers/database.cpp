#include "Smile2Unlock/backend/managers/database.h"
#include <algorithm>
#include <ctime>

namespace smile2unlock {
namespace managers {

Database::Database() : next_user_id_(1), next_face_id_(1) {
    // 不再初始化假数据
}

std::string GetCurrentTime() {
    time_t now = time(nullptr);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&now));
    return std::string(buf);
}

std::vector<User> Database::GetAllUsers() const {
    return users_;
}

std::optional<User> Database::GetUserById(int id) const {
    auto it = std::find_if(users_.begin(), users_.end(),
                          [id](const User& u) { return u.id == id; });
    
    if (it != users_.end()) {
        return *it;
    }
    return std::nullopt;
}

std::optional<User> Database::GetUserByUsername(const std::string& username) const {
    auto it = std::find_if(users_.begin(), users_.end(),
                          [&username](const User& u) { return u.username == username; });
    
    if (it != users_.end()) {
        return *it;
    }
    return std::nullopt;
}

bool Database::AddUser(const User& user) {
    if (UserExists(user.username)) {
        return false;
    }
    
    User new_user = user;
    new_user.id = next_user_id_++;
    new_user.created_at = GetCurrentTime();
    
    users_.push_back(new_user);
    return true;
}

bool Database::UpdateUser(const User& user) {
    auto it = std::find_if(users_.begin(), users_.end(),
                          [&user](const User& u) { return u.id == user.id; });
    
    if (it != users_.end()) {
        it->username = user.username;
        it->encrypted_password = user.encrypted_password;
        it->remark = user.remark;
        // 保持faces和created_at不变
        return true;
    }
    return false;
}

bool Database::DeleteUser(int id) {
    auto it = std::remove_if(users_.begin(), users_.end(),
                            [id](const User& u) { return u.id == id; });
    
    if (it != users_.end()) {
        users_.erase(it, users_.end());
        return true;
    }
    return false;
}

bool Database::UserExists(const std::string& username) const {
    return GetUserByUsername(username).has_value();
}

bool Database::AddFace(int user_id, const FaceData& face) {
    auto it = std::find_if(users_.begin(), users_.end(),
                          [user_id](const User& u) { return u.id == user_id; });
    
    if (it != users_.end()) {
        FaceData new_face = face;
        new_face.id = next_face_id_++;
        new_face.created_at = GetCurrentTime();
        it->faces.push_back(new_face);
        return true;
    }
    return false;
}

bool Database::DeleteFace(int user_id, int face_id) {
    auto user_it = std::find_if(users_.begin(), users_.end(),
                                [user_id](const User& u) { return u.id == user_id; });
    
    if (user_it != users_.end()) {
        auto& faces = user_it->faces;
        auto face_it = std::remove_if(faces.begin(), faces.end(),
                                      [face_id](const FaceData& f) { return f.id == face_id; });
        
        if (face_it != faces.end()) {
            faces.erase(face_it, faces.end());
            return true;
        }
    }
    return false;
}

bool Database::UpdateFace(int user_id, const FaceData& face) {
    auto user_it = std::find_if(users_.begin(), users_.end(),
                                [user_id](const User& u) { return u.id == user_id; });
    
    if (user_it != users_.end()) {
        auto& faces = user_it->faces;
        auto face_it = std::find_if(faces.begin(), faces.end(),
                                    [&face](const FaceData& f) { return f.id == face.id; });
        
        if (face_it != faces.end()) {
            face_it->remark = face.remark;
            // 保持feature, image_path, created_at不变
            return true;
        }
    }
    return false;
}

std::vector<FaceData> Database::GetUserFaces(int user_id) const {
    auto user = GetUserById(user_id);
    if (user.has_value()) {
        return user->faces;
    }
    return {};
}

std::optional<User> Database::FindUserByFace(const std::string& feature) const {
    // TODO: 实现特征向量相似度匹配
    // 这里需要调用 FaceRecognition 的 CompareFaces 方法
    return std::nullopt;
}

} // namespace managers
} // namespace smile2unlock
