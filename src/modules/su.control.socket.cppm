module;

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

export module su.control.socket;

import std;

export namespace su::control {

inline constexpr std::string_view kDefaultSocketPath = "/run/smile2unlock/control.sock";
inline constexpr std::uint32_t kProtocolVersion = 1;
inline constexpr std::size_t kMaximumFrameSize = 16 * 1024;

enum class SocketError {
    kInvalidArgument,
    kPathTooLong,
    kCreateFailed,
    kBindFailed,
    kListenFailed,
    kConnectFailed,
    kAcceptFailed,
    kReadFailed,
    kWriteFailed,
    kProtocolError,
};

enum class ControlResult {
    kAccepted,
    kRejected,
    kUnavailable,
    kCancelled,
    kBusy,
    kError,
};

struct ControlResponse {
    std::uint64_t request_id = 0;
    ControlResult result = ControlResult::kError;
};

class Connection {
public:
    Connection() = default;
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;

    static std::expected<Connection, SocketError> connect_to(
        std::string_view path,
        std::chrono::seconds timeout = std::chrono::seconds{15});

    std::expected<void, SocketError> send_frame(std::string_view payload) const;
    std::expected<std::string, SocketError> receive_frame() const;
    std::expected<std::uint32_t, SocketError> peer_uid() const;
    [[nodiscard]] bool valid() const { return fd_ >= 0; }

private:
    explicit Connection(int fd) : fd_(fd) {}
    void reset();

    int fd_ = -1;
    friend class Listener;
};

class Listener {
public:
    Listener() = default;
    ~Listener();
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;
    Listener(Listener&& other) noexcept;
    Listener& operator=(Listener&& other) noexcept;

    static std::expected<Listener, SocketError> bind_to(std::string_view path);
    std::expected<Connection, SocketError> accept_one() const;

private:
    Listener(int fd, std::string path) : fd_(fd), path_(std::move(path)) {}
    void reset();

    int fd_ = -1;
    std::string path_;
};

std::string make_authenticate_request(std::uint64_t request_id, std::string_view username);
std::string make_status_request(std::uint64_t request_id);
std::string make_cancel_request(std::uint64_t request_id, std::uint64_t target_request_id);
std::string make_response(
    std::uint64_t request_id,
    ControlResult result,
    std::string_view reason = {});
std::expected<ControlResponse, SocketError> parse_response(std::string_view response);

} // namespace su::control

namespace su::control {

namespace {

std::expected<sockaddr_un, SocketError> socket_address(std::string_view path) {
    if (path.empty()) {
        return std::unexpected(SocketError::kInvalidArgument);
    }
    auto address = sockaddr_un{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) {
        return std::unexpected(SocketError::kPathTooLong);
    }
    std::memcpy(address.sun_path, path.data(), path.size());
    address.sun_path[path.size()] = '\0';
    return address;
}

bool set_timeout(int fd, std::chrono::seconds timeout) {
    const auto value = timeval{.tv_sec = timeout.count(), .tv_usec = 0};
    return ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value)) == 0
        && ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value)) == 0;
}

std::expected<void, SocketError> write_all(int fd, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::byte*>(data);
    auto written = std::size_t{0};
    while (written < size) {
        const auto result = ::send(fd, bytes + written, size - written, MSG_NOSIGNAL);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return std::unexpected(SocketError::kWriteFailed);
        }
        written += static_cast<std::size_t>(result);
    }
    return {};
}

std::expected<void, SocketError> read_all(int fd, void* data, std::size_t size) {
    auto* bytes = static_cast<std::byte*>(data);
    auto consumed = std::size_t{0};
    while (consumed < size) {
        const auto result = ::recv(fd, bytes + consumed, size - consumed, 0);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return std::unexpected(SocketError::kReadFailed);
        }
        consumed += static_cast<std::size_t>(result);
    }
    return {};
}

std::string json_escape(std::string_view value) {
    auto output = std::string{};
    output.reserve(value.size());
    for (const auto character : value) {
        switch (character) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (static_cast<unsigned char>(character) < 0x20) {
                output += std::format("\\u{:04x}", static_cast<unsigned char>(character));
            } else {
                output += character;
            }
        }
    }
    return output;
}

std::string_view result_name(ControlResult result) {
    switch (result) {
    case ControlResult::kAccepted: return "accepted";
    case ControlResult::kRejected: return "rejected";
    case ControlResult::kUnavailable: return "unavailable";
    case ControlResult::kCancelled: return "cancelled";
    case ControlResult::kBusy: return "busy";
    case ControlResult::kError: return "error";
    }
    std::unreachable();
}

} // namespace

Connection::~Connection() {
    reset();
}

Connection::Connection(Connection&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)) {}

Connection& Connection::operator=(Connection&& other) noexcept {
    if (this != &other) {
        reset();
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

void Connection::reset() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

std::expected<Connection, SocketError> Connection::connect_to(
    std::string_view path,
    std::chrono::seconds timeout) {
    const auto address = socket_address(path);
    if (!address) {
        return std::unexpected(address.error());
    }
    const auto fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return std::unexpected(SocketError::kCreateFailed);
    }
    auto connection = Connection{fd};
    if (!set_timeout(fd, timeout)
        || ::connect(fd, reinterpret_cast<const sockaddr*>(&*address), sizeof(*address)) != 0) {
        return std::unexpected(SocketError::kConnectFailed);
    }
    return connection;
}

std::expected<void, SocketError> Connection::send_frame(std::string_view payload) const {
    if (!valid() || payload.empty() || payload.size() > kMaximumFrameSize
        || !std::in_range<std::uint32_t>(payload.size())) {
        return std::unexpected(SocketError::kInvalidArgument);
    }
    const auto length = ::htonl(static_cast<std::uint32_t>(payload.size()));
    if (auto header = write_all(fd_, &length, sizeof(length)); !header) {
        return header;
    }
    return write_all(fd_, payload.data(), payload.size());
}

std::expected<std::string, SocketError> Connection::receive_frame() const {
    if (!valid()) {
        return std::unexpected(SocketError::kInvalidArgument);
    }
    auto network_length = std::uint32_t{0};
    if (auto header = read_all(fd_, &network_length, sizeof(network_length)); !header) {
        return std::unexpected(header.error());
    }
    const auto length = static_cast<std::size_t>(::ntohl(network_length));
    if (length == 0 || length > kMaximumFrameSize) {
        return std::unexpected(SocketError::kProtocolError);
    }
    auto payload = std::string(length, '\0');
    if (auto body = read_all(fd_, payload.data(), payload.size()); !body) {
        return std::unexpected(body.error());
    }
    return payload;
}

std::expected<std::uint32_t, SocketError> Connection::peer_uid() const {
    auto credentials = ucred{};
    auto length = socklen_t{sizeof(credentials)};
    if (::getsockopt(fd_, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0
        || length != sizeof(credentials)) {
        return std::unexpected(SocketError::kProtocolError);
    }
    return static_cast<std::uint32_t>(credentials.uid);
}

Listener::~Listener() {
    reset();
}

Listener::Listener(Listener&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), path_(std::move(other.path_)) {}

Listener& Listener::operator=(Listener&& other) noexcept {
    if (this != &other) {
        reset();
        fd_ = std::exchange(other.fd_, -1);
        path_ = std::move(other.path_);
    }
    return *this;
}

void Listener::reset() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (!path_.empty()) {
        ::unlink(path_.c_str());
        path_.clear();
    }
}

std::expected<Listener, SocketError> Listener::bind_to(std::string_view path) {
    const auto address = socket_address(path);
    if (!address) {
        return std::unexpected(address.error());
    }
    const auto fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return std::unexpected(SocketError::kCreateFailed);
    }
    auto listener = Listener{fd, std::string(path)};
    ::unlink(listener.path_.c_str());
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&*address), sizeof(*address)) != 0) {
        return std::unexpected(SocketError::kBindFailed);
    }
    // Only root-owned PAM processes may connect; SO_PEERCRED is checked again
    // after accept so filesystem permissions are not the sole trust boundary.
    if (::chmod(listener.path_.c_str(), 0600) != 0 || ::listen(fd, 16) != 0) {
        return std::unexpected(SocketError::kListenFailed);
    }
    return listener;
}

std::expected<Connection, SocketError> Listener::accept_one() const {
    const auto fd = ::accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd < 0) {
        return std::unexpected(SocketError::kAcceptFailed);
    }
    auto connection = Connection{fd};
    if (!set_timeout(fd, std::chrono::seconds{15})) {
        return std::unexpected(SocketError::kAcceptFailed);
    }
    return connection;
}

std::string make_authenticate_request(std::uint64_t request_id, std::string_view username) {
    return std::format(
        R"({{"version":{},"msg_type":"authenticate","request_id":{},"username":"{}"}})",
        kProtocolVersion,
        request_id,
        json_escape(username));
}

std::string make_status_request(std::uint64_t request_id) {
    return std::format(
        R"({{"version":{},"msg_type":"status","request_id":{}}})",
        kProtocolVersion,
        request_id);
}

std::string make_cancel_request(std::uint64_t request_id, std::uint64_t target_request_id) {
    return std::format(
        R"({{"version":{},"msg_type":"cancel","request_id":{},"target_request_id":{}}})",
        kProtocolVersion,
        request_id,
        target_request_id);
}

std::string make_response(
    std::uint64_t request_id,
    ControlResult result,
    std::string_view reason) {
    return std::format(
        R"({{"version":{},"msg_type":"auth_result","request_id":{},"result":"{}","reason":"{}"}})",
        kProtocolVersion,
        request_id,
        result_name(result),
        json_escape(reason));
}

std::expected<ControlResponse, SocketError> parse_response(std::string_view response) {
    if (!response.contains(R"("version":1,)")
        || !response.contains(R"("msg_type":"auth_result")")) {
        return std::unexpected(SocketError::kProtocolError);
    }

    constexpr auto request_id_field = std::string_view{R"("request_id":)"};
    const auto request_id_start = response.find(request_id_field);
    if (request_id_start == std::string_view::npos) {
        return std::unexpected(SocketError::kProtocolError);
    }
    const auto digits = response.substr(request_id_start + request_id_field.size());
    auto request_id = std::uint64_t{0};
    const auto parsed = std::from_chars(
        digits.data(), digits.data() + digits.size(), request_id);
    if (parsed.ec != std::errc{}
        || parsed.ptr == digits.data()
        || parsed.ptr == digits.data() + digits.size()
        || *parsed.ptr != ','
        || request_id == 0) {
        return std::unexpected(SocketError::kProtocolError);
    }

    for (const auto result : {
             ControlResult::kAccepted,
             ControlResult::kRejected,
             ControlResult::kUnavailable,
             ControlResult::kCancelled,
             ControlResult::kBusy,
             ControlResult::kError,
         }) {
        const auto token = std::format(R"("result":"{}")", result_name(result));
        if (response.contains(token)) {
            return ControlResponse{.request_id = request_id, .result = result};
        }
    }
    return std::unexpected(SocketError::kProtocolError);
}

} // namespace su::control
