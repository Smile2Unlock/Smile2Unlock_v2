module;

#include <sqlite3.h>
#include <ctime>

export module smile2unlock.database;

import std;
import smile2unlock.models;

namespace smile2unlock::managers {

std::string GetCurrentTimeStr() {
    time_t now = time(nullptr);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&now));
    return std::string(buf);
}

}

export namespace smile2unlock::managers {

class Database {
public:
    Database();
    ~Database();

    bool Initialize(const std::string& db_path = "smile2unlock.db");

    std::vector<User> GetAllUsers() const;
    std::optional<User> GetUserById(int id) const;
    std::optional<User> GetUserByUsername(const std::string& username) const;
    bool AddUser(const User& user);
    bool UpdateUser(const User& user);
    bool DeleteUser(int id);
    bool UserExists(const std::string& username) const;

    bool AddFace(int user_id, const FaceData& face);
    bool DeleteFace(int user_id, int face_id);
    bool UpdateFace(int user_id, const FaceData& face);
    std::vector<FaceData> GetUserFaces(int user_id) const;

    std::optional<User> FindUserByFace(const std::string& feature) const;

private:
    bool CreateTables();
    void ResetSequenceIfTableEmpty(const char* table_name);
    sqlite3* db_;
};

} // namespace smile2unlock::managers

module :private;

namespace smile2unlock::managers {

Database::Database() : db_(nullptr) {}

Database::~Database() {
    if (db_) {
        sqlite3_close(db_);
    }
}

bool Database::Initialize(const std::string& db_path) {
    if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
        std::cerr << "Cannot open database: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    return CreateTables();
}

bool Database::CreateTables() {
    char* err_msg = nullptr;
    const char* user_table_sql =
        "CREATE TABLE IF NOT EXISTS users ("
        "id INTEGER PRIMARY KEY,"
        "username TEXT UNIQUE NOT NULL,"
        "encrypted_password TEXT,"
        "remark TEXT,"
        "created_at TEXT"
        ");";

    const char* face_table_sql =
        "CREATE TABLE IF NOT EXISTS faces ("
        "id INTEGER PRIMARY KEY,"
        "user_id INTEGER NOT NULL,"
        "feature TEXT,"
        "image_path TEXT,"
        "remark TEXT,"
        "created_at TEXT,"
        "FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE"
        ");";

    if (sqlite3_exec(db_, user_table_sql, 0, 0, &err_msg) != SQLITE_OK) {
        std::cerr << "Failed to create users table: " << (err_msg ? err_msg : "") << std::endl;
        if (err_msg) sqlite3_free(err_msg);
        return false;
    }

    if (sqlite3_exec(db_, face_table_sql, 0, 0, &err_msg) != SQLITE_OK) {
        std::cerr << "Failed to create faces table: " << (err_msg ? err_msg : "") << std::endl;
        if (err_msg) sqlite3_free(err_msg);
        return false;
    }

    sqlite3_exec(db_, "PRAGMA foreign_keys = ON;", 0, 0, 0);
    return true;
}

std::vector<User> Database::GetAllUsers() const {
    std::vector<User> users;
    const char* sql = "SELECT id, username, encrypted_password, remark, created_at FROM users;";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            User u;
            u.id = sqlite3_column_int(stmt, 0);
            u.username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            const char* pwd = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            if (pwd) u.encrypted_password = pwd;
            const char* rmk = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            if (rmk) u.remark = rmk;
            const char* cat = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            if (cat) u.created_at = cat;

            u.faces = GetUserFaces(u.id);
            users.push_back(u);
        }
    }
    sqlite3_finalize(stmt);
    return users;
}

std::optional<User> Database::GetUserById(int id) const {
    const char* sql = "SELECT id, username, encrypted_password, remark, created_at FROM users WHERE id = ?;";
    sqlite3_stmt* stmt;
    std::optional<User> user = std::nullopt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            User u;
            u.id = sqlite3_column_int(stmt, 0);
            u.username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            const char* pwd = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            if (pwd) u.encrypted_password = pwd;
            const char* rmk = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            if (rmk) u.remark = rmk;
            const char* cat = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            if (cat) u.created_at = cat;

            u.faces = GetUserFaces(u.id);
            user = u;
        }
    }
    sqlite3_finalize(stmt);
    return user;
}

std::optional<User> Database::GetUserByUsername(const std::string& username) const {
    const char* sql = "SELECT id, username, encrypted_password, remark, created_at FROM users WHERE username = ?;";
    sqlite3_stmt* stmt;
    std::optional<User> user = std::nullopt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            User u;
            u.id = sqlite3_column_int(stmt, 0);
            u.username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            const char* pwd = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            if (pwd) u.encrypted_password = pwd;
            const char* rmk = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            if (rmk) u.remark = rmk;
            const char* cat = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            if (cat) u.created_at = cat;

            u.faces = GetUserFaces(u.id);
            user = u;
        }
    }
    sqlite3_finalize(stmt);
    return user;
}

bool Database::AddUser(const User& user) {
    if (UserExists(user.username)) return false;

    const char* sql = "INSERT INTO users (username, encrypted_password, remark, created_at) VALUES (?, ?, ?, ?);";
    sqlite3_stmt* stmt;
    bool success = false;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, user.username.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, user.encrypted_password.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, user.remark.c_str(), -1, SQLITE_TRANSIENT);
        std::string now = GetCurrentTimeStr();
        sqlite3_bind_text(stmt, 4, now.c_str(), -1, SQLITE_TRANSIENT);

        if (sqlite3_step(stmt) == SQLITE_DONE) {
            success = true;
        }
    }
    sqlite3_finalize(stmt);
    return success;
}

bool Database::UpdateUser(const User& user) {
    const char* sql = "UPDATE users SET username=?, encrypted_password=?, remark=? WHERE id=?;";
    sqlite3_stmt* stmt;
    bool success = false;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, user.username.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, user.encrypted_password.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, user.remark.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 4, user.id);

        if (sqlite3_step(stmt) == SQLITE_DONE) {
            success = true;
        }
    }
    sqlite3_finalize(stmt);
    return success;
}

bool Database::DeleteUser(int id) {
    const char* sql = "DELETE FROM users WHERE id=?;";
    sqlite3_stmt* stmt;
    bool success = false;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, id);
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            success = sqlite3_changes(db_) > 0;
        }
    }
    sqlite3_finalize(stmt);
    if (success) {
        ResetSequenceIfTableEmpty("faces");
        ResetSequenceIfTableEmpty("users");
    }
    return success;
}

bool Database::UserExists(const std::string& username) const {
    return GetUserByUsername(username).has_value();
}

bool Database::AddFace(int user_id, const FaceData& face) {
    const char* sql = "INSERT INTO faces (user_id, feature, image_path, remark, created_at) VALUES (?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt;
    bool success = false;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, user_id);
        sqlite3_bind_text(stmt, 2, face.feature.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, face.image_path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, face.remark.c_str(), -1, SQLITE_TRANSIENT);
        std::string now = GetCurrentTimeStr();
        sqlite3_bind_text(stmt, 5, now.c_str(), -1, SQLITE_TRANSIENT);

        if (sqlite3_step(stmt) == SQLITE_DONE) {
            success = true;
        }
    }
    sqlite3_finalize(stmt);
    return success;
}

bool Database::DeleteFace(int user_id, int face_id) {
    const char* sql = "DELETE FROM faces WHERE id=? AND user_id=?;";
    sqlite3_stmt* stmt;
    bool success = false;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, face_id);
        sqlite3_bind_int(stmt, 2, user_id);
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            success = sqlite3_changes(db_) > 0;
        }
    }
    sqlite3_finalize(stmt);
    if (success) {
        ResetSequenceIfTableEmpty("faces");
    }
    return success;
}

bool Database::UpdateFace(int user_id, const FaceData& face) {
    const char* sql = "UPDATE faces SET feature=?, image_path=?, remark=? WHERE id=? AND user_id=?;";
    sqlite3_stmt* stmt;
    bool success = false;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, face.feature.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, face.image_path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, face.remark.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 4, face.id);
        sqlite3_bind_int(stmt, 5, user_id);

        if (sqlite3_step(stmt) == SQLITE_DONE) {
            success = true;
        }
    }
    sqlite3_finalize(stmt);
    return success;
}

std::vector<FaceData> Database::GetUserFaces(int user_id) const {
    std::vector<FaceData> faces;
    const char* sql = "SELECT id, feature, image_path, remark, created_at FROM faces WHERE user_id = ?;";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, user_id);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            FaceData f;
            f.id = sqlite3_column_int(stmt, 0);
            const char* feat = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            if (feat) f.feature = feat;
            const char* path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            if (path) f.image_path = path;
            const char* rmk = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            if (rmk) f.remark = rmk;
            const char* cat = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            if (cat) f.created_at = cat;
            faces.push_back(f);
        }
    }
    sqlite3_finalize(stmt);
    return faces;
}

std::optional<User> Database::FindUserByFace(const std::string& feature) const {
    return std::nullopt;
}

void Database::ResetSequenceIfTableEmpty(const char* table_name) {
    if (table_name == nullptr || db_ == nullptr) {
        return;
    }

    const std::string count_sql = "SELECT COUNT(*) FROM " + std::string(table_name) + ";";
    sqlite3_stmt* stmt = nullptr;
    bool is_empty = false;

    if (sqlite3_prepare_v2(db_, count_sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            is_empty = sqlite3_column_int(stmt, 0) == 0;
        }
    }
    sqlite3_finalize(stmt);

    if (!is_empty) {
        return;
    }

    sqlite3_stmt* reset_stmt = nullptr;
    const char* reset_sql = "DELETE FROM sqlite_sequence WHERE name = ?;";
    if (sqlite3_prepare_v2(db_, reset_sql, -1, &reset_stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(reset_stmt, 1, table_name, -1, SQLITE_TRANSIENT);
        sqlite3_step(reset_stmt);
    }
    sqlite3_finalize(reset_stmt);
}

} // namespace smile2unlock::managers
