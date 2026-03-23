#pragma once

#include "Smile2Unlock/backend/models/types.h"
#include <vector>
#include <string>
#include <optional>
#include <sqlite3.h>

namespace smile2unlock {
namespace managers {

class Database {
public:
    Database();
    ~Database();

    bool Initialize(const std::string& db_path = "smile2unlock.db");

    // 用户CRUD
    std::vector<User> GetAllUsers() const;
    std::optional<User> GetUserById(int id) const;
    std::optional<User> GetUserByUsername(const std::string& username) const;
    bool AddUser(const User& user);
    bool UpdateUser(const User& user);
    bool DeleteUser(int id);
    bool UserExists(const std::string& username) const;

    // 人脸CRUD
    bool AddFace(int user_id, const FaceData& face);
    bool DeleteFace(int user_id, int face_id);
    bool UpdateFace(int user_id, const FaceData& face);
    std::vector<FaceData> GetUserFaces(int user_id) const;
    
    // 人脸识别
    std::optional<User> FindUserByFace(const std::string& feature) const;

private:
    bool CreateTables();
    sqlite3* db_;
};

} // namespace managers
} // namespace smile2unlock
