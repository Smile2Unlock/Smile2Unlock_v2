#include "user_store.hpp"

#include <algorithm>
#include <chrono>
#include <format>

#include <tb_database.h>

namespace rewrite::backend {

namespace {

constexpr std::string_view kSchema = R"(
CREATE TABLE IF NOT EXISTS users (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    username TEXT UNIQUE NOT NULL,
    password_hash TEXT NOT NULL,
    remark TEXT DEFAULT '',
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS faces (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER NOT NULL,
    feature BLOB NOT NULL,
    remark TEXT DEFAULT '',
    created_at INTEGER NOT NULL,
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_faces_user_id ON faces(user_id);
)";

class UserStoreImpl : public UserStore {
public:
    explicit UserStoreImpl(tb_database_t db) : db_(db) {}

    std::expected<std::vector<User>, std::string> get_all_users() override {
        std::vector<User> users;
        auto stmt = tb_database_prepare(db_, "SELECT id, username, password_hash, remark, created_at, updated_at FROM users ORDER BY id");
        if (!stmt) {
            return std::unexpected("failed to prepare statement");
        }
        while (tb_database_step(stmt)) {
            User u;
            u.id = tb_database_column_int64(stmt, 0);
            u.username = tb_database_column_text(stmt, 1) ? tb_database_column_text(stmt, 1) : "";
            u.password_hash = tb_database_column_text(stmt, 2) ? tb_database_column_text(stmt, 2) : "";
            u.remark = tb_database_column_text(stmt, 3) ? tb_database_column_text(stmt, 3) : "";
            u.created_at = tb_database_column_int64(stmt, 4);
            u.updated_at = tb_database_column_int64(stmt, 5);
            users.push_back(u);
        }
        return users;
    }

    std::expected<User, std::string> get_user_by_id(std::uint64_t id) override {
        auto stmt = tb_database_prepare(db_, "SELECT id, username, password_hash, remark, created_at, updated_at FROM users WHERE id = ?");
        if (!stmt) {
            return std::unexpected("failed to prepare statement");
        }
        tb_database_bind_int64(stmt, 1, id);
        if (tb_database_step(stmt)) {
            User u;
            u.id = tb_database_column_int64(stmt, 0);
            u.username = tb_database_column_text(stmt, 1) ? tb_database_column_text(stmt, 1) : "";
            u.password_hash = tb_database_column_text(stmt, 2) ? tb_database_column_text(stmt, 2) : "";
            u.remark = tb_database_column_text(stmt, 3) ? tb_database_column_text(stmt, 3) : "";
            u.created_at = tb_database_column_int64(stmt, 4);
            u.updated_at = tb_database_column_int64(stmt, 5);
            return u;
        }
        return std::unexpected(std::format("user {} not found", id));
    }

    std::expected<User, std::string> get_user_by_username(std::string_view username) override {
        auto stmt = tb_database_prepare(db_, "SELECT id, username, password_hash, remark, created_at, updated_at FROM users WHERE username = ?");
        if (!stmt) {
            return std::unexpected("failed to prepare statement");
        }
        tb_database_bind_text(stmt, 1, username.data(), username.size());
        if (tb_database_step(stmt)) {
            User u;
            u.id = tb_database_column_int64(stmt, 0);
            u.username = tb_database_column_text(stmt, 1) ? tb_database_column_text(stmt, 1) : "";
            u.password_hash = tb_database_column_text(stmt, 2) ? tb_database_column_text(stmt, 2) : "";
            u.remark = tb_database_column_text(stmt, 3) ? tb_database_column_text(stmt, 3) : "";
            u.created_at = tb_database_column_int64(stmt, 4);
            u.updated_at = tb_database_column_int64(stmt, 5);
            return u;
        }
        return std::unexpected(std::format("user '{}' not found", username));
    }

    std::expected<User, std::string> add_user(std::string_view username, std::string_view password_hash, std::string_view remark) override {
        auto now = static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
        auto stmt = tb_database_prepare(db_, "INSERT INTO users (username, password_hash, remark, created_at, updated_at) VALUES (?, ?, ?, ?, ?)");
        if (!stmt) {
            return std::unexpected("failed to prepare statement");
        }
        tb_database_bind_text(stmt, 1, username.data(), username.size());
        tb_database_bind_text(stmt, 2, password_hash.data(), password_hash.size());
        tb_database_bind_text(stmt, 3, remark.data(), remark.size());
        tb_database_bind_int64(stmt, 4, now);
        tb_database_bind_int64(stmt, 5, now);
        if (tb_database_execute(stmt)) {
            return get_user_by_username(username);
        }
        return std::unexpected("failed to insert user");
    }

    std::expected<User, std::string> update_user(std::uint64_t id, std::string_view username, std::string_view password_hash, std::string_view remark) override {
        auto now = static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
        auto stmt = tb_database_prepare(db_, "UPDATE users SET username = ?, password_hash = ?, remark = ?, updated_at = ? WHERE id = ?");
        if (!stmt) {
            return std::unexpected("failed to prepare statement");
        }
        tb_database_bind_text(stmt, 1, username.data(), username.size());
        tb_database_bind_text(stmt, 2, password_hash.data(), password_hash.size());
        tb_database_bind_text(stmt, 3, remark.data(), remark.size());
        tb_database_bind_int64(stmt, 4, now);
        tb_database_bind_int64(stmt, 5, id);
        if (tb_database_execute(stmt)) {
            return get_user_by_id(id);
        }
        return std::unexpected(std::format("failed to update user {}", id));
    }

    std::expected<void, std::string> delete_user(std::uint64_t id) override {
        auto stmt = tb_database_prepare(db_, "DELETE FROM users WHERE id = ?");
        if (!stmt) {
            return std::unexpected("failed to prepare statement");
        }
        tb_database_bind_int64(stmt, 1, id);
        if (!tb_database_execute(stmt)) {
            return std::unexpected(std::format("failed to delete user {}", id));
        }
        return {};
    }

    std::expected<std::vector<FaceRecord>, std::string> get_faces_for_user(std::uint64_t user_id) override {
        std::vector<FaceRecord> faces;
        auto stmt = tb_database_prepare(db_, "SELECT id, user_id, feature, remark, created_at FROM faces WHERE user_id = ? ORDER BY id");
        if (!stmt) {
            return std::unexpected("failed to prepare statement");
        }
        tb_database_bind_int64(stmt, 1, user_id);
        while (tb_database_step(stmt)) {
            FaceRecord f;
            f.id = tb_database_column_int64(stmt, 0);
            f.user_id = tb_database_column_int64(stmt, 1);
            const void* blob = tb_database_column_blob(stmt, 2);
            std::size_t blob_size = tb_database_column_bytes(stmt, 2);
            f.feature.resize(blob_size / sizeof(float));
            std::memcpy(f.feature.data(), blob, blob_size);
            f.remark = tb_database_column_text(stmt, 3) ? tb_database_column_text(stmt, 3) : "";
            f.created_at = tb_database_column_int64(stmt, 4);
            faces.push_back(f);
        }
        return faces;
    }

    std::expected<FaceRecord, std::string> add_face(std::uint64_t user_id, std::span<const float> feature, std::string_view remark) override {
        auto now = static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
        auto stmt = tb_database_prepare(db_, "INSERT INTO faces (user_id, feature, remark, created_at) VALUES (?, ?, ?, ?)");
        if (!stmt) {
            return std::unexpected("failed to prepare statement");
        }
        tb_database_bind_int64(stmt, 1, user_id);
        tb_database_bind_blob(stmt, 2, feature.data(), feature.size() * sizeof(float));
        tb_database_bind_text(stmt, 3, remark.data(), remark.size());
        tb_database_bind_int64(stmt, 4, now);
        if (!tb_database_execute(stmt)) {
            return std::unexpected("failed to insert face");
        }

        auto get_stmt = tb_database_prepare(db_, "SELECT id, user_id, feature, remark, created_at FROM faces WHERE user_id = ? ORDER BY id DESC LIMIT 1");
        if (!get_stmt) {
            return std::unexpected("failed to get inserted face");
        }
        tb_database_bind_int64(get_stmt, 1, user_id);
        if (tb_database_step(get_stmt)) {
            FaceRecord f;
            f.id = tb_database_column_int64(get_stmt, 0);
            f.user_id = tb_database_column_int64(get_stmt, 1);
            const void* blob = tb_database_column_blob(get_stmt, 2);
            std::size_t blob_size = tb_database_column_bytes(get_stmt, 2);
            f.feature.resize(blob_size / sizeof(float));
            std::memcpy(f.feature.data(), blob, blob_size);
            f.remark = tb_database_column_text(get_stmt, 3) ? tb_database_column_text(get_stmt, 3) : "";
            f.created_at = tb_database_column_int64(get_stmt, 4);
            return f;
        }
        return std::unexpected("failed to retrieve inserted face");
    }

    std::expected<void, std::string> delete_face(std::uint64_t user_id, std::uint64_t face_id) override {
        auto stmt = tb_database_prepare(db_, "DELETE FROM faces WHERE id = ? AND user_id = ?");
        if (!stmt) {
            return std::unexpected("failed to prepare statement");
        }
        tb_database_bind_int64(stmt, 1, face_id);
        tb_database_bind_int64(stmt, 2, user_id);
        if (!tb_database_execute(stmt)) {
            return std::unexpected(std::format("failed to delete face {} for user {}", face_id, user_id));
        }
        return {};
    }

private:
    tb_database_t db_;
};

} // namespace

std::expected<std::unique_ptr<UserStore>, std::string> create_user_store() {
    const std::filesystem::path db_path = [] {
        const char* env = std::getenv("SMILE2UNLOCK_DB");
        if (env && *env) {
            return std::filesystem::path(env);
        }
#ifdef _WIN32
        if (const char* localappdata = std::getenv("LOCALAPPDATA")) {
            return std::filesystem::path(localappdata) / "Smile2Unlock" / "smile2unlock.db";
        }
        return std::filesystem::temp_directory_path() / "Smile2Unlock" / "smile2unlock.db";
#else
        return std::filesystem::path("/var/lib/smile2unlock") / "smile2unlock.db";
#endif
    }();

    std::error_code ec;
    std::filesystem::create_directories(db_path.parent_path(), ec);
    if (ec) {
        return std::unexpected(std::format("failed to create database directory: {}", ec.message()));
    }

    tb_database_t db = tb_database_init(db_path.string().c_str(), kSchema, nullptr);
    if (!db) {
        return std::unexpected(std::format("failed to open database at {}", db_path.string()));
    }

    return std::unique_ptr<UserStore>(new UserStoreImpl(db));
}

} // namespace rewrite::backend