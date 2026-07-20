#pragma once

#include "windows/logon_secret_protocol.h"

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

class PreparedPipePassword {
public:
    PreparedPipePassword(const PreparedPipePassword&) = delete;
    PreparedPipePassword& operator=(const PreparedPipePassword&) = delete;
    PreparedPipePassword(PreparedPipePassword&& other) noexcept;
    PreparedPipePassword& operator=(PreparedPipePassword&& other) noexcept;
    ~PreparedPipePassword();

    [[nodiscard]] PCWSTR c_str() const { return password_.data(); }
    [[nodiscard]] std::size_t size() const { return size_; }

private:
    friend class LogonSecretClient;
    PreparedPipePassword() = default;
    void clear() noexcept;

    std::array<wchar_t, smile2unlock::logon_secret_ipc::kPasswordCapacity> password_{};
    std::size_t size_ = 0;
};

class LogonSecretClient {
public:
    std::expected<PreparedPipePassword, HRESULT> prepare(
        PCWSTR sid,
        std::uint64_t request_id,
        std::uint32_t logon_session_id) const;
    HRESULT mark_stale(
        PCWSTR sid,
        std::uint64_t request_id,
        std::uint32_t logon_session_id) const;
    HRESULT store_for_current_user(std::span<const wchar_t> password) const;
    HRESULT clear_for_current_user() const;

private:
    std::expected<std::wstring, HRESULT> current_user_sid() const;
    std::expected<smile2unlock::logon_secret_ipc::Response, HRESULT> transact(
        smile2unlock::logon_secret_ipc::Request& request) const;
};
