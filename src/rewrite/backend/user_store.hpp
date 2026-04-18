#pragma once

#include "backend_core.hpp"

#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace rewrite::backend {

struct User {
    std::uint64_t id{0};
    std::string username;
    std::string password_hash;
    std::string remark;
    std::uint64_t created_at{0};
    std::uint64_t updated_at{0};
};

struct FaceRecord {
    std::uint64_t id{0};
    std::uint64_t user_id{0};
    std::vector<float> feature; // float32le binary
    std::string remark;
    std::uint64_t created_at{0};
};

struct UserStore {
    virtual ~UserStore() = default;

    virtual std::expected<std::vector<User>, std::string> get_all_users() = 0;
    virtual std::expected<User, std::string> get_user_by_id(std::uint64_t id) = 0;
    virtual std::expected<User, std::string> get_user_by_username(std::string_view username) = 0;
    virtual std::expected<User, std::string> add_user(std::string_view username, std::string_view password_hash, std::string_view remark) = 0;
    virtual std::expected<User, std::string> update_user(std::uint64_t id, std::string_view username, std::string_view password_hash, std::string_view remark) = 0;
    virtual std::expected<void, std::string> delete_user(std::uint64_t id) = 0;

    virtual std::expected<std::vector<FaceRecord>, std::string> get_faces_for_user(std::uint64_t user_id) = 0;
    virtual std::expected<FaceRecord, std::string> add_face(std::uint64_t user_id, std::span<const float> feature, std::string_view remark) = 0;
    virtual std::expected<void, std::string> delete_face(std::uint64_t user_id, std::uint64_t face_id) = 0;
};

std::expected<std::unique_ptr<UserStore>, std::string> create_user_store();

} // namespace rewrite::backend