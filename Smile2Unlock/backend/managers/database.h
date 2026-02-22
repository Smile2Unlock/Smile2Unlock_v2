#pragma once

#include "Smile2Unlock/backend/models/types.h"
#include <vector>
#include <string>
#include <optional>

namespace smile2unlock {
namespace managers {

class Database {
public:
    Database();
    ~Database() = default;

    std::vector<User> GetAllUsers() const;
    std::optional<User> GetUserById(int id) const;
    std::optional<User> GetUserByUsername(const std::string& username) const;
    bool AddUser(const User& user);
    bool UpdateUser(const User& user);
    bool DeleteUser(int id);
    bool UserExists(const std::string& username) const;

private:
    void InitializeFakeData();
    
    std::vector<User> users_;
    int next_user_id_;
};

} // namespace managers
} // namespace smile2unlock
