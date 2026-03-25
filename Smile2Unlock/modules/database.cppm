module;

#include <sqlite3.h>
#include <ctime>

export module smile2unlock.database;

import std;
import crypto;
import smile2unlock.models;

namespace smile2unlock::managers {

std::string GetCurrentTimeStr() {
    time_t now = time(nullptr);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&now));
    return std::string(buf);
}

constexpr std::string_view kPasswordPrefix = "aes256cbc:";
constexpr std::string_view kKeyFileHeader = "SMILE2UNLOCK_KEY_V1";

std::filesystem::path ResolveAbsolutePath(const std::string& path) {
    std::filesystem::path fs_path(path);
    if (fs_path.is_absolute()) {
        return fs_path;
    }
    return std::filesystem::current_path() / fs_path;
}

std::filesystem::path GetKeyFilePath(const std::filesystem::path& db_path) {
    auto key_path = db_path;
    key_path.replace_extension(".key");
    return key_path;
}

std::string TrimWhitespace(std::string value) {
    auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    while (!value.empty() && is_space(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && is_space(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::string LoadOrCreateStorageKeyHex(const std::filesystem::path& key_path) {
    std::error_code ec;
    if (std::filesystem::exists(key_path, ec)) {
        std::ifstream input(key_path, std::ios::binary);
        if (!input.is_open()) {
            return {};
        }

        std::string header;
        std::getline(input, header);
        std::string key_hex;
        std::getline(input, key_hex);
        header = TrimWhitespace(header);
        key_hex = TrimWhitespace(key_hex);
        if (header != kKeyFileHeader || key_hex.empty()) {
            return {};
        }
        return key_hex;
    }

    if (key_path.has_parent_path()) {
        std::filesystem::create_directories(key_path.parent_path(), ec);
    }

    Crypto crypto;
    const std::string key_hex = crypto.getKeyHex();

    std::ofstream output(key_path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        return {};
    }

    output << kKeyFileHeader << '\n' << key_hex << '\n';
    output.close();
    return key_hex;
}

std::optional<Crypto> CreateStorageCrypto(const std::filesystem::path& db_path, std::string* iv_hex = nullptr) {
    Crypto crypto;
    const std::string key_hex = LoadOrCreateStorageKeyHex(GetKeyFilePath(db_path));
    if (key_hex.empty() || !crypto.setKeyHex(key_hex)) {
        return std::nullopt;
    }
    if (iv_hex != nullptr) {
        *iv_hex = crypto.getIvHex();
    }
    return crypto;
}

std::string EncryptPasswordForStorage(std::string_view plain_password, const std::string& db_path) {
    std::string iv_hex;
    auto crypto = CreateStorageCrypto(ResolveAbsolutePath(db_path), &iv_hex);
    if (!crypto.has_value() || iv_hex.empty()) {
        return {};
    }

    const std::string cipher_bytes = crypto->encrypt(plain_password);
    if (cipher_bytes.empty()) {
        return {};
    }

    const std::string cipher_hex = Crypto::bytesToHex(std::span<const CryptoPP::byte>(
        reinterpret_cast<const CryptoPP::byte*>(cipher_bytes.data()), cipher_bytes.size()));
    return std::string(kPasswordPrefix) + iv_hex + ":" + cipher_hex;
}

std::string DecryptPasswordFromStorage(std::string_view stored_password, const std::string& db_path) {
    if (!stored_password.starts_with(kPasswordPrefix)) {
        return std::string(stored_password);
    }

    const std::string_view payload = stored_password.substr(kPasswordPrefix.size());
    const size_t separator = payload.find(':');
    if (separator == std::string_view::npos) {
        return {};
    }

    const std::string iv_hex(payload.substr(0, separator));
    const std::string cipher_hex(payload.substr(separator + 1));
    if (iv_hex.empty() || cipher_hex.empty()) {
        return {};
    }

    auto crypto = CreateStorageCrypto(ResolveAbsolutePath(db_path));
    if (!crypto.has_value() || !crypto->setIvHex(iv_hex)) {
        return {};
    }

    const std::vector<CryptoPP::byte> cipher_bytes = Crypto::hexToBytes(cipher_hex);
    if (cipher_bytes.empty()) {
        return {};
    }

    const std::string cipher(reinterpret_cast<const char*>(cipher_bytes.data()), cipher_bytes.size());
    return crypto->decrypt(cipher);
}

}

export namespace smile2unlock::managers {

std::string EncryptPasswordForStorage(std::string_view plain_password, const std::string& db_path);
std::string DecryptPasswordFromStorage(std::string_view stored_password, const std::string& db_path);

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
    std::string DecryptPassword(const std::string& stored_password) const;

    bool AddFace(int user_id, const FaceData& face);
    bool DeleteFace(int user_id, int face_id);
    bool UpdateFace(int user_id, const FaceData& face);
    std::vector<FaceData> GetUserFaces(int user_id) const;

    std::optional<User> FindUserByFace(const std::string& feature) const;

private:
    bool CreateTables();
    void ResetSequenceIfTableEmpty(const char* table_name);
    std::string db_path_;
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
    db_path_ = ResolveAbsolutePath(db_path).string();
    if (sqlite3_open(db_path_.c_str(), &db_) != SQLITE_OK) {
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

std::string Database::DecryptPassword(const std::string& stored_password) const {
    return DecryptPasswordFromStorage(stored_password, db_path_);
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
