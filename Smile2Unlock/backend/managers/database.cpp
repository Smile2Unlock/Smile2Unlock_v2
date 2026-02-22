#include "Smile2Unlock/backend/managers/database.h"
#include <algorithm>

namespace smile2unlock {
namespace managers {

Database::Database() : next_user_id_(4) {
    InitializeFakeData();
}

void Database::InitializeFakeData() {
    users_.push_back({1, "admin", "FAKE_FEATURE_ADMIN", "encrypted_admin_password"});
    users_.push_back({2, "user1", "FAKE_FEATURE_USER1", "encrypted_user1_password"});
    users_.push_back({3, "test", "FAKE_FEATURE_TEST", "encrypted_test_password"});
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
    if (new_user.id == 0) {
        new_user.id = next_user_id_++;
    }
    
    users_.push_back(new_user);
    return true;
}

bool Database::UpdateUser(const User& user) {
    auto it = std::find_if(users_.begin(), users_.end(),
                          [&user](const User& u) { return u.id == user.id; });
    
    if (it != users_.end()) {
        *it = user;
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

} // namespace managers
} // namespace smile2unlock
