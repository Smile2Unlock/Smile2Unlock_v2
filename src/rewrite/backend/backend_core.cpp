#include "rewrite/backend_core.hpp"

#include <chrono>
#include <cstring>
#include <format>
#include <memory>
#include <utility>

#include <tbox/tbox.h>

import rewrite.protocol;
import rewrite.runtime;
import rewrite.socket;

namespace rewrite::backend {

namespace {

class UserStore {
public:
    explicit UserStore(tb_database_sql_ref_t db) : db_(db) {}

    std::expected<std::vector<User>, std::string> get_all_users() {
        std::vector<User> users;
        if (!tb_database_sql_done(db_, "SELECT id, username, password_hash, remark, created_at, updated_at FROM users ORDER BY id")) {
            return std::unexpected("failed to execute get_all_users query");
        }
        auto result = tb_database_sql_result_load(db_, true);
        if (!result) {
            return std::unexpected("failed to load users result");
        }
        tb_for_all_if(tb_iterator_ref_t, row, result, row) {
            User u;
            tb_database_sql_value_t const* id = static_cast<tb_database_sql_value_t const*>(tb_iterator_item(row, 0));
            tb_database_sql_value_t const* username = static_cast<tb_database_sql_value_t const*>(tb_iterator_item(row, 1));
            tb_database_sql_value_t const* password_hash = static_cast<tb_database_sql_value_t const*>(tb_iterator_item(row, 2));
            tb_database_sql_value_t const* remark = static_cast<tb_database_sql_value_t const*>(tb_iterator_item(row, 3));
            tb_database_sql_value_t const* created_at = static_cast<tb_database_sql_value_t const*>(tb_iterator_item(row, 4));
            tb_database_sql_value_t const* updated_at = static_cast<tb_database_sql_value_t const*>(tb_iterator_item(row, 5));
            if (id) u.id = tb_database_sql_value_int64(id);
            if (username) u.username = tb_database_sql_value_text(username) ? tb_database_sql_value_text(username) : "";
            if (password_hash) u.password_hash = tb_database_sql_value_text(password_hash) ? tb_database_sql_value_text(password_hash) : "";
            if (remark) u.remark = tb_database_sql_value_text(remark) ? tb_database_sql_value_text(remark) : "";
            if (created_at) u.created_at = tb_database_sql_value_int64(created_at);
            if (updated_at) u.updated_at = tb_database_sql_value_int64(updated_at);
            users.push_back(u);
        }
        tb_database_sql_result_exit(db_, result);
        return users;
    }

    std::expected<User, std::string> get_user_by_id(std::uint64_t id) {
        auto stmt = tb_database_sql_statement_init(db_, "SELECT id, username, password_hash, remark, created_at, updated_at FROM users WHERE id = ?");
        if (!stmt) {
            return std::unexpected("failed to prepare get_user_by_id statement");
        }
        tb_database_sql_value_t arg = {0};
        tb_database_sql_value_set_int64(&arg, id);
        tb_database_sql_statement_bind(db_, stmt, &arg, 1);
        if (!tb_database_sql_statement_done(db_, stmt)) {
            tb_database_sql_statement_exit(db_, stmt);
            return std::unexpected("failed to execute get_user_by_id");
        }
        User u;
        auto result = tb_database_sql_result_load(db_, true);
        if (result) {
            tb_for_all_if(tb_iterator_ref_t, row, result, row) {
                tb_database_sql_value_t const* col0 = static_cast<tb_database_sql_value_t const*>(tb_iterator_item(row, 0));
                tb_database_sql_value_t const* col1 = static_cast<tb_database_sql_value_t const*>(tb_iterator_item(row, 1));
                tb_database_sql_value_t const* col2 = static_cast<tb_database_sql_value_t const*>(tb_iterator_item(row, 2));
                tb_database_sql_value_t const* col3 = static_cast<tb_database_sql_value_t const*>(tb_iterator_item(row, 3));
                tb_database_sql_value_t const* col4 = static_cast<tb_database_sql_value_t const*>(tb_iterator_item(row, 4));
                tb_database_sql_value_t const* col5 = static_cast<tb_database_sql_value_t const*>(tb_iterator_item(row, 5));
                if (col0) u.id = tb_database_sql_value_int64(col0);
                if (col1) u.username = tb_database_sql_value_text(col1) ? tb_database_sql_value_text(col1) : "";
                if (col2) u.password_hash = tb_database_sql_value_text(col2) ? tb_database_sql_value_text(col2) : "";
                if (col3) u.remark = tb_database_sql_value_text(col3) ? tb_database_sql_value_text(col3) : "";
                if (col4) u.created_at = tb_database_sql_value_int64(col4);
                if (col5) u.updated_at = tb_database_sql_value_int64(col5);
            }
            tb_database_sql_result_exit(db_, result);
        }
        tb_database_sql_statement_exit(db_, stmt);
        if (u.id == 0) {
            return std::unexpected(std::format("user {} not found", id));
        }
        return u;
    }

    std::expected<User, std::string> get_user_by_username(std::string_view username) {
        auto stmt = tb_database_sql_statement_init(db_, "SELECT id, username, password_hash, remark, created_at, updated_at FROM users WHERE username = ?");
        if (!stmt) {
            return std::unexpected("failed to prepare get_user_by_username statement");
        }
        tb_database_sql_value_t arg = {0};
        tb_database_sql_value_set_text(&arg, username.data(), username.size());
        tb_database_sql_statement_bind(db_, stmt, &arg, 1);
        if (!tb_database_sql_statement_done(db_, stmt)) {
            tb_database_sql_statement_exit(db_, stmt);
            return std::unexpected("failed to execute get_user_by_username");
        }
        User u;
        auto result = tb_database_sql_result_load(db_, true);
        if (result) {
            tb_for_all_if(tb_iterator_ref_t, row, result, row) {
                tb_database_sql_value_t const* col0 = static_cast<tb_database_sql_value_t const*>(tb_iterator_item(row, 0));
                tb_database_sql_value_t const* col1 = static_cast<tb_database_sql_value_t const*>(tb_iterator_item(row, 1));
                tb_database_sql_value_t const* col2 = static_cast<tb_database_sql_value_t const*>(tb_iterator_item(row, 2));
                tb_database_sql_value_t const* col3 = static_cast<tb_database_sql_value_t const*>(tb_iterator_item(row, 3));
                tb_database_sql_value_t const* col4 = static_cast<tb_database_sql_value_t const*>(tb_iterator_item(row, 4));
                tb_database_sql_value_t const* col5 = static_cast<tb_database_sql_value_t const*>(tb_iterator_item(row, 5));
                if (col0) u.id = tb_database_sql_value_int64(col0);
                if (col1) u.username = tb_database_sql_value_text(col1) ? tb_database_sql_value_text(col1) : "";
                if (col2) u.password_hash = tb_database_sql_value_text(col2) ? tb_database_sql_value_text(col2) : "";
                if (col3) u.remark = tb_database_sql_value_text(col3) ? tb_database_sql_value_text(col3) : "";
                if (col4) u.created_at = tb_database_sql_value_int64(col4);
                if (col5) u.updated_at = tb_database_sql_value_int64(col5);
            }
            tb_database_sql_result_exit(db_, result);
        }
        tb_database_sql_statement_exit(db_, stmt);
        if (u.id == 0) {
            return std::unexpected(std::format("user '{}' not found", username));
        }
        return u;
    }

    std::expected<User, std::string> add_user(std::string_view username, std::string_view password_hash, std::string_view remark) {
        auto now = static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
        auto stmt = tb_database_sql_statement_init(db_, "INSERT INTO users (username, password_hash, remark, created_at, updated_at) VALUES (?, ?, ?, ?, ?)");
        if (!stmt) {
            return std::unexpected("failed to prepare add_user statement");
        }
        tb_database_sql_value_t args[5] = {{0}};
        tb_database_sql_value_set_text(&args[0], username.data(), username.size());
        tb_database_sql_value_set_text(&args[1], password_hash.data(), password_hash.size());
        tb_database_sql_value_set_text(&args[2], remark.data(), remark.size());
        tb_database_sql_value_set_int64(&args[3], now);
        tb_database_sql_value_set_int64(&args[4], now);
        tb_database_sql_statement_bind(db_, stmt, args, 5);
        if (!tb_database_sql_statement_done(db_, stmt)) {
            tb_database_sql_statement_exit(db_, stmt);
            return std::unexpected("failed to insert user");
        }
        tb_database_sql_statement_exit(db_, stmt);
        return get_user_by_username(username);
    }

    std::expected<User, std::string> update_user(std::uint64_t id, std::string_view username, std::string_view password_hash, std::string_view remark) {
        auto now = static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
        auto stmt = tb_database_sql_statement_init(db_, "UPDATE users SET username = ?, password_hash = ?, remark = ?, updated_at = ? WHERE id = ?");
        if (!stmt) {
            return std::unexpected("failed to prepare update_user statement");
        }
        tb_database_sql_value_t args[5] = {{0}};
        tb_database_sql_value_set_text(&args[0], username.data(), username.size());
        tb_database_sql_value_set_text(&args[1], password_hash.data(), password_hash.size());
        tb_database_sql_value_set_text(&args[2], remark.data(), remark.size());
        tb_database_sql_value_set_int64(&args[3], now);
        tb_database_sql_value_set_int64(&args[4], id);
        tb_database_sql_statement_bind(db_, stmt, args, 5);
        if (!tb_database_sql_statement_done(db_, stmt)) {
            tb_database_sql_statement_exit(db_, stmt);
            return std::unexpected(std::format("failed to update user {}", id));
        }
        tb_database_sql_statement_exit(db_, stmt);
        return get_user_by_id(id);
    }

    std::expected<void, std::string> delete_user(std::uint64_t id) {
        auto stmt = tb_database_sql_statement_init(db_, "DELETE FROM users WHERE id = ?");
        if (!stmt) {
            return std::unexpected("failed to prepare delete_user statement");
        }
        tb_database_sql_value_t arg = {0};
        tb_database_sql_value_set_int64(&arg, id);
        tb_database_sql_statement_bind(db_, stmt, &arg, 1);
        if (!tb_database_sql_statement_done(db_, stmt)) {
            tb_database_sql_statement_exit(db_, stmt);
            return std::unexpected(std::format("failed to delete user {}", id));
        }
        tb_database_sql_statement_exit(db_, stmt);
        return {};
    }

    std::expected<std::vector<FaceRecord>, std::string> get_faces_for_user(std::uint64_t user_id) {
        std::vector<FaceRecord> faces;
        auto stmt = tb_database_sql_statement_init(db_, "SELECT id, user_id, feature, remark, created_at FROM faces WHERE user_id = ? ORDER BY id");
        if (!stmt) {
            return std::unexpected("failed to prepare get_faces_for_user statement");
        }
        tb_database_sql_value_t arg = {0};
        tb_database_sql_value_set_int64(&arg, user_id);
        tb_database_sql_statement_bind(db_, stmt, &arg, 1);
        if (!tb_database_sql_statement_done(db_, stmt)) {
            tb_database_sql_statement_exit(db_, stmt);
            return std::unexpected("failed to execute get_faces_for_user");
        }
        auto result = tb_database_sql_result_load(db_, true);
        if (result) {
            tb_for_all_if(tb_iterator_ref_t, row, result, row) {
                FaceRecord f;
                tb_database_sql_value_t const* col0 = static_cast<tb_database_sql_value_t const*>(tb_iterator_item(row, 0));
                tb_database_sql_value_t const* col1 = static_cast<tb_database_sql_value_t const*>(tb_iterator_item(row, 1));
                tb_database_sql_value_t const* col2 = static_cast<tb_database_sql_value_t const*>(tb_iterator_item(row, 2));
                tb_database_sql_value_t const* col3 = static_cast<tb_database_sql_value_t const*>(tb_iterator_item(row, 3));
                tb_database_sql_value_t const* col4 = static_cast<tb_database_sql_value_t const*>(tb_iterator_item(row, 4));
                if (col0) f.id = tb_database_sql_value_int64(col0);
                if (col1) f.user_id = tb_database_sql_value_int64(col1);
                if (col2 && tb_database_sql_value_blob(col2)) {
                    std::size_t blob_size = tb_database_sql_value_size(col2);
                    f.feature.resize(blob_size / sizeof(float));
                    std::memcpy(f.feature.data(), tb_database_sql_value_blob(col2), blob_size);
                }
                if (col3) f.remark = tb_database_sql_value_text(col3) ? tb_database_sql_value_text(col3) : "";
                if (col4) f.created_at = tb_database_sql_value_int64(col4);
                faces.push_back(f);
            }
            tb_database_sql_result_exit(db_, result);
        }
        tb_database_sql_statement_exit(db_, stmt);
        return faces;
    }

    std::expected<FaceRecord, std::string> add_face(std::uint64_t user_id, std::span<const float> feature, std::string_view remark) {
        auto now = static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
        auto stmt = tb_database_sql_statement_init(db_, "INSERT INTO faces (user_id, feature, remark, created_at) VALUES (?, ?, ?, ?)");
        if (!stmt) {
            return std::unexpected("failed to prepare add_face statement");
        }
        tb_database_sql_value_t args[4] = {{0}};
        tb_database_sql_value_set_int64(&args[0], user_id);
        tb_database_sql_value_set_blob8(&args[1], reinterpret_cast<const tb_byte_t*>(feature.data()), feature.size() * sizeof(float));
        tb_database_sql_value_set_text(&args[2], remark.data(), remark.size());
        tb_database_sql_value_set_int64(&args[3], now);
        tb_database_sql_statement_bind(db_, stmt, args, 4);
        if (!tb_database_sql_statement_done(db_, stmt)) {
            tb_database_sql_statement_exit(db_, stmt);
            return std::unexpected("failed to insert face");
        }
        tb_database_sql_statement_exit(db_, stmt);

        auto get_stmt = tb_database_sql_statement_init(db_, "SELECT id, user_id, feature, remark, created_at FROM faces WHERE user_id = ? ORDER BY id DESC LIMIT 1");
        if (!get_stmt) {
            return std::unexpected("failed to prepare get_face statement");
        }
        tb_database_sql_value_t get_arg = {0};
        tb_database_sql_value_set_int64(&get_arg, user_id);
        tb_database_sql_statement_bind(db_, get_stmt, &get_arg, 1);
        FaceRecord f;
        if (tb_database_sql_statement_done(db_, get_stmt)) {
            auto result = tb_database_sql_result_load(db_, true);
            if (result) {
                tb_for_all_if(tb_iterator_ref_t, row, result, row) {
                    tb_database_sql_value_t const* col0 = static_cast<tb_database_sql_value_t const*>(tb_iterator_item(row, 0));
                    tb_database_sql_value_t const* col1 = static_cast<tb_database_sql_value_t const*>(tb_iterator_item(row, 1));
                    tb_database_sql_value_t const* col2 = static_cast<tb_database_sql_value_t const*>(tb_iterator_item(row, 2));
                    tb_database_sql_value_t const* col3 = static_cast<tb_database_sql_value_t const*>(tb_iterator_item(row, 3));
                    tb_database_sql_value_t const* col4 = static_cast<tb_database_sql_value_t const*>(tb_iterator_item(row, 4));
                    if (col0) f.id = tb_database_sql_value_int64(col0);
                    if (col1) f.user_id = tb_database_sql_value_int64(col1);
                    if (col2 && tb_database_sql_value_blob(col2)) {
                        std::size_t blob_size = tb_database_sql_value_size(col2);
                        f.feature.resize(blob_size / sizeof(float));
                        std::memcpy(f.feature.data(), tb_database_sql_value_blob(col2), blob_size);
                    }
                    if (col3) f.remark = tb_database_sql_value_text(col3) ? tb_database_sql_value_text(col3) : "";
                    if (col4) f.created_at = tb_database_sql_value_int64(col4);
                }
                tb_database_sql_result_exit(db_, result);
            }
        }
        tb_database_sql_statement_exit(db_, get_stmt);
        if (f.id == 0) {
            return std::unexpected("failed to retrieve inserted face");
        }
        return f;
    }

    std::expected<void, std::string> delete_face(std::uint64_t user_id, std::uint64_t face_id) {
        auto stmt = tb_database_sql_statement_init(db_, "DELETE FROM faces WHERE id = ? AND user_id = ?");
        if (!stmt) {
            return std::unexpected("failed to prepare delete_face statement");
        }
        tb_database_sql_value_t args[2] = {{0}};
        tb_database_sql_value_set_int64(&args[0], face_id);
        tb_database_sql_value_set_int64(&args[1], user_id);
        tb_database_sql_statement_bind(db_, stmt, args, 2);
        if (!tb_database_sql_statement_done(db_, stmt)) {
            tb_database_sql_statement_exit(db_, stmt);
            return std::unexpected(std::format("failed to delete face {} for user {}", face_id, user_id));
        }
        tb_database_sql_statement_exit(db_, stmt);
        return {};
    }

private:
    tb_database_sql_ref_t db_;
};

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

} // namespace

struct BackendCore::Impl {
    rewrite::runtime::EndpointSet runtime_endpoints{rewrite::runtime::default_endpoints()};
    EndpointSet public_endpoints{
        .control = runtime_endpoints.control,
        .events = runtime_endpoints.events,
        .fr_control = runtime_endpoints.fr_control,
        .preview = runtime_endpoints.preview
    };
    RuntimeState state{false, false, {}};
    rewrite::net::SocketServer control_server{{runtime_endpoints.control}};
    rewrite::net::SocketServer event_server{{runtime_endpoints.events}};
    rewrite::net::SocketServer fr_control_server{{runtime_endpoints.fr_control}};
    rewrite::net::SocketServer preview_server{{runtime_endpoints.preview}};
    std::unique_ptr<UserStore> user_store;
    tb_database_sql_ref_t database{nullptr};
    BackendConfig config{};
};

BackendCore::BackendCore()
    : impl_(new Impl()) {}

BackendCore::~BackendCore() {
    if (impl_->database) {
        tb_database_sql_exit(impl_->database);
    }
    delete impl_;
}

BackendCore::BackendCore(BackendCore&& other) noexcept
    : impl_(std::exchange(other.impl_, nullptr)) {}

BackendCore& BackendCore::operator=(BackendCore&& other) noexcept {
    if (this != &other) {
        delete impl_;
        impl_ = std::exchange(other.impl_, nullptr);
    }
    return *this;
}

std::expected<void, std::string> BackendCore::initialize() {
    if (impl_->state.initialized) {
        return {};
    }

    if (auto dirs = rewrite::runtime::ensure_runtime_dirs(); !dirs) {
        return std::unexpected(dirs.error());
    }

    const std::string db_url = [] -> std::string {
        const char* env = std::getenv("SMILE2UNLOCK_DB");
        if (env && *env) {
            return std::string("file://") + env;
        }
#ifdef _WIN32
        if (const char* localappdata = std::getenv("LOCALAPPDATA")) {
            return std::format("file://{}/Smile2Unlock/smile2unlock.db", localappdata);
        }
        return "file://" + (std::filesystem::temp_directory_path() / "Smile2Unlock" / "smile2unlock.db").string();
#else
        return std::string("file:///var/lib/smile2unlock/smile2unlock.db");
#endif
    }();

    impl_->database = tb_database_sql_init(db_url.c_str());
    if (!impl_->database) {
        return std::unexpected(std::format("failed to initialize database: {}", db_url));
    }

    if (!tb_database_sql_open(impl_->database)) {
        tb_database_sql_exit(impl_->database);
        impl_->database = nullptr;
        return std::unexpected("failed to open database");
    }

    impl_->user_store = std::unique_ptr<UserStore>(new UserStore(impl_->database));

    impl_->state.initialized = true;
    impl_->state.summary = "rewrite backend core initialized";
    return {};
}

std::expected<void, std::string> BackendCore::start_listeners() {
    if (!impl_->state.initialized) {
        return std::unexpected("backend core is not initialized");
    }

    if (auto control = impl_->control_server.bind_and_listen(); !control) {
        return std::unexpected(std::format("control listener failed: {}", control.error()));
    }
    if (auto events = impl_->event_server.bind_and_listen(); !events) {
        return std::unexpected(std::format("event listener failed: {}", events.error()));
    }
    if (auto fr = impl_->fr_control_server.bind_and_listen(); !fr) {
        return std::unexpected(std::format("fr-control listener failed: {}", fr.error()));
    }
    if (auto preview = impl_->preview_server.bind_and_listen(); !preview) {
        return std::unexpected(std::format("preview listener failed: {}", preview.error()));
    }

    impl_->state.running = true;
    impl_->state.summary = "rewrite backend listeners active";
    return {};
}

std::string BackendCore::status_json() const {
    return std::format(
        "{{\"initialized\":{},\"running\":{},\"control\":\"{}\",\"events\":\"{}\",\"fr_control\":\"{}\",\"preview\":\"{}\",\"summary\":\"{}\"}}",
        impl_->state.initialized ? "true" : "false",
        impl_->state.running ? "true" : "false",
        impl_->public_endpoints.control,
        impl_->public_endpoints.events,
        impl_->public_endpoints.fr_control,
        impl_->public_endpoints.preview,
        impl_->state.summary);
}

const RuntimeState& BackendCore::state() const noexcept {
    return impl_->state;
}

const EndpointSet& BackendCore::endpoints() const noexcept {
    return impl_->public_endpoints;
}

std::expected<std::vector<User>, std::string> BackendCore::get_all_users() {
    if (!impl_->user_store) {
        return std::unexpected("user store not initialized");
    }
    return impl_->user_store->get_all_users();
}

std::expected<User, std::string> BackendCore::get_user(std::uint64_t id) {
    if (!impl_->user_store) {
        return std::unexpected("user store not initialized");
    }
    return impl_->user_store->get_user_by_id(id);
}

std::expected<User, std::string> BackendCore::add_user(std::string_view username, std::string_view password_hash, std::string_view remark) {
    if (!impl_->user_store) {
        return std::unexpected("user store not initialized");
    }
    return impl_->user_store->add_user(username, password_hash, remark);
}

std::expected<User, std::string> BackendCore::update_user(std::uint64_t id, std::string_view username, std::string_view password_hash, std::string_view remark) {
    if (!impl_->user_store) {
        return std::unexpected("user store not initialized");
    }
    return impl_->user_store->update_user(id, username, password_hash, remark);
}

std::expected<void, std::string> BackendCore::delete_user(std::uint64_t id) {
    if (!impl_->user_store) {
        return std::unexpected("user store not initialized");
    }
    return impl_->user_store->delete_user(id);
}

std::expected<std::vector<FaceRecord>, std::string> BackendCore::get_user_faces(std::uint64_t user_id) {
    if (!impl_->user_store) {
        return std::unexpected("user store not initialized");
    }
    return impl_->user_store->get_faces_for_user(user_id);
}

std::expected<FaceRecord, std::string> BackendCore::add_user_face(std::uint64_t user_id, std::span<const float> feature, std::string_view remark) {
    if (!impl_->user_store) {
        return std::unexpected("user store not initialized");
    }
    return impl_->user_store->add_face(user_id, feature, remark);
}

std::expected<void, std::string> BackendCore::delete_user_face(std::uint64_t user_id, std::uint64_t face_id) {
    if (!impl_->user_store) {
        return std::unexpected("user store not initialized");
    }
    return impl_->user_store->delete_face(user_id, face_id);
}

std::expected<BackendConfig, std::string> BackendCore::get_config() {
    return impl_->config;
}

std::expected<void, std::string> BackendCore::set_config(const BackendConfig& config) {
    impl_->config = config;
    return {};
}

std::expected<void, std::string> BackendCore::start_preview(int camera, std::string_view codec, int max_fps, int width, int height) {
    if (!impl_->state.running) {
        return std::unexpected("backend not running");
    }
    return std::unexpected("FR client not yet implemented");
}

std::expected<void, std::string> BackendCore::stop_preview() {
    if (!impl_->state.running) {
        return std::unexpected("backend not running");
    }
    return std::unexpected("FR client not yet implemented");
}

std::expected<BackendCore::CaptureResult, std::string> BackendCore::capture_frame(bool want_feature) {
    if (!impl_->state.running) {
        return std::unexpected("backend not running");
    }
    return std::unexpected("FR client not yet implemented");
}

std::expected<BackendCore::RecognitionResult, std::string> BackendCore::start_recognition(std::string_view username_hint, bool interactive) {
    if (!impl_->state.running) {
        return std::unexpected("backend not running");
    }
    return std::unexpected("FR client not yet implemented");
}

std::expected<void, std::string> BackendCore::stop_recognition(std::uint64_t session_id) {
    if (!impl_->state.running) {
        return std::unexpected("backend not running");
    }
    return std::unexpected("FR client not yet implemented");
}

} // namespace rewrite::backend