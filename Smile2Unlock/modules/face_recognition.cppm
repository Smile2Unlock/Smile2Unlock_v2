module;

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <boost/asio.hpp>
#include "models/udp_auth_request_packet.h"
#include "models/recognition_status.h"
#include "models/udp_status_packet.h"
#include "models/shared_frame_ipc.h"

export module smile2unlock.face_recognition;

import std;
import smile2unlock.models;

namespace asio = boost::asio;
using udp = asio::ip::udp;

namespace fs = std::filesystem;

namespace smile2unlock::managers {
namespace {
constexpr DWORD kSharedFrameMappingSize =
    sizeof(smile2unlock::SharedFrameHeader) + smile2unlock::SharedFrameHeader::MAX_IMAGE_BYTES;

class UdpReceiverFromFR {
public:
    using StatusCallback = std::function<void(RecognitionStatus, const std::string&)>;
    explicit UdpReceiverFromFR(uint16_t port = 51235) : socket_(io_context_, udp::endpoint(udp::v4(), port)), running_(false) {}
    ~UdpReceiverFromFR() { stop(); }
    void set_callback(StatusCallback callback) { callback_ = callback; }
    void start() { if (!running_) { running_ = true; recv_thread_ = std::thread([this]() { receive_loop(); }); } }
    void stop() {
        if (!running_) return;
        running_ = false;
        boost::system::error_code ec;
        socket_.close(ec);
        if (recv_thread_.joinable()) recv_thread_.join();
    }
private:
    void receive_loop() {
        UdpStatusPacket packet;
        udp::endpoint sender_endpoint;
        while (running_) {
            try {
                boost::system::error_code ec;
                size_t len = socket_.receive_from(asio::buffer(&packet, sizeof(packet)), sender_endpoint, 0, ec);
                if (ec == asio::error::operation_aborted || ec == asio::error::bad_descriptor) break;
                if (ec || len != sizeof(UdpStatusPacket)) continue;
                if (packet.magic_number != MAGIC_NUMBER || packet.version != PROTOCOL_VERSION) continue;
                if (callback_) callback_(static_cast<RecognitionStatus>(packet.status_code), std::string(packet.username));
            } catch (const std::exception&) {}
        }
    }
    asio::io_context io_context_;
    udp::socket socket_;
    std::atomic<bool> running_;
    std::thread recv_thread_;
    StatusCallback callback_;
};

class UdpSenderToCP {
public:
    UdpSenderToCP(const std::string& host = "127.0.0.1", uint16_t port = 51234)
        : socket_(io_context_, udp::v4()), endpoint_(asio::ip::make_address(host), port) {}
    ~UdpSenderToCP() { boost::system::error_code ec; socket_.close(ec); }
    bool send_status(RecognitionStatus status, const std::string& username = "") {
        try {
            UdpStatusPacket packet;
            packet.magic_number = MAGIC_NUMBER;
            packet.version = PROTOCOL_VERSION;
            packet.status_code = static_cast<int32_t>(status);
            memset(packet.username, 0, sizeof(packet.username));
            if (!username.empty()) strncpy_s(packet.username, sizeof(packet.username), username.c_str(), _TRUNCATE);
            packet.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            socket_.send_to(asio::buffer(&packet, sizeof(packet)), endpoint_);
            return true;
        } catch (const std::exception&) { return false; }
    }
private:
    asio::io_context io_context_;
    udp::socket socket_;
    udp::endpoint endpoint_;
};

class UdpReceiverFromCP {
public:
    using RequestCallback = std::function<void(AuthRequestType, const std::string&, uint32_t)>;
    explicit UdpReceiverFromCP(uint16_t port = 51236) : socket_(io_context_, udp::endpoint(udp::v4(), port)), running_(false) {}
    ~UdpReceiverFromCP() { stop(); }
    void set_callback(RequestCallback callback) { callback_ = callback; }
    void start() { if (!running_) { running_ = true; recv_thread_ = std::thread([this]() { receive_loop(); }); } }
    void stop() {
        if (!running_) return;
        running_ = false;
        boost::system::error_code ec;
        socket_.close(ec);
        if (recv_thread_.joinable()) recv_thread_.join();
    }
private:
    void receive_loop() {
        UdpAuthRequestPacket packet;
        udp::endpoint sender_endpoint;
        while (running_) {
            try {
                boost::system::error_code ec;
                size_t len = socket_.receive_from(asio::buffer(&packet, sizeof(packet)), sender_endpoint, 0, ec);
                if (ec == asio::error::operation_aborted || ec == asio::error::bad_descriptor) break;
                if (ec || len != sizeof(UdpAuthRequestPacket)) continue;
                if (packet.magic_number != AUTH_REQUEST_MAGIC || packet.version != AUTH_REQUEST_VERSION) continue;
                if (callback_) callback_(static_cast<AuthRequestType>(packet.request_type), std::string(packet.username_hint), packet.session_id);
            } catch (const std::exception&) {
                if (running_) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }
    asio::io_context io_context_;
    udp::socket socket_;
    std::thread recv_thread_;
    std::atomic<bool> running_;
    RequestCallback callback_;
};
}
}

export namespace smile2unlock::managers {

class FaceRecognition {
public:
    FaceRecognition();
    ~FaceRecognition();

    bool Initialize(std::string& error_message);
    bool IsInitialized() const { return initialized_; }

    bool StartRecognition();
    void StopRecognition();
    bool IsRunning() const { return is_running_; }
    bool CaptureFrameImage(std::vector<unsigned char>& image_data, int& width, int& height, std::string& error_message);

    RecognitionResult GetLastResult() const;
    std::string ExtractFaceFeature(const std::string& image_path);
    bool CompareFaces(const std::string& feature1, const std::string& feature2, float& similarity);

private:
    bool initialized_;
    bool is_running_;
    RecognitionResult last_result_;

    HANDLE fr_process_;
    PROCESS_INFORMATION fr_process_info_;
    HANDLE fr_stdout_read_;
    HANDLE fr_stderr_read_;
    std::thread log_thread_;
    bool log_thread_running_;

    void ReadSubProcessLogs();

    std::unique_ptr<UdpReceiverFromCP> udp_receiver_cp_;
    std::unique_ptr<UdpReceiverFromFR> udp_receiver_fr_;
    std::unique_ptr<UdpSenderToCP> udp_sender_;

    uint32_t current_session_id_;
    std::string pending_username_hint_;

    bool start_fr_process();
    bool start_fr_capture_process(const std::string& map_name, std::string& error_message);
    void stop_fr_process();
    void on_cp_request_received(AuthRequestType type, const std::string& username_hint, uint32_t session_id);
    void on_fr_status_received(RecognitionStatus status, const std::string& username);
};

} // namespace smile2unlock::managers

module :private;

namespace smile2unlock::managers {

FaceRecognition::FaceRecognition()
    : initialized_(false), is_running_(false), fr_process_(nullptr), current_session_id_(0),
      fr_stdout_read_(nullptr), fr_stderr_read_(nullptr), log_thread_running_(false) {}

FaceRecognition::~FaceRecognition() {
    StopRecognition();
    stop_fr_process();
}

bool FaceRecognition::start_fr_process() {
    char exe_path[MAX_PATH];
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    fs::path fr_exe = fs::path(exe_path).parent_path() / "FaceRecognizer.exe";
    if (!fs::exists(fr_exe)) {
        std::cerr << "[FR Manager] FaceRecognizer.exe 不存在: " << fr_exe << std::endl;
        return false;
    }

    SECURITY_ATTRIBUTES saAttr{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE hStdoutWrite = NULL;
    HANDLE hStderrWrite = NULL;
    if (!CreatePipe(&fr_stdout_read_, &hStdoutWrite, &saAttr, 0)) return false;
    SetHandleInformation(fr_stdout_read_, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&fr_stderr_read_, &hStderrWrite, &saAttr, 0)) {
        CloseHandle(hStdoutWrite);
        return false;
    }
    SetHandleInformation(fr_stderr_read_, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.hStdError = hStderrWrite;
    si.hStdOutput = hStdoutWrite;
    si.dwFlags |= STARTF_USESTDHANDLES;
    memset(&fr_process_info_, 0, sizeof(fr_process_info_));

    std::string cmd_line = "\"" + fr_exe.string() + "\" -m recognize --udp-port 51235";
    if (!CreateProcessA(nullptr, cmd_line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &fr_process_info_)) {
        CloseHandle(hStdoutWrite);
        CloseHandle(hStderrWrite);
        return false;
    }

    fr_process_ = fr_process_info_.hProcess;
    CloseHandle(hStdoutWrite);
    CloseHandle(hStderrWrite);
    log_thread_running_ = true;
    log_thread_ = std::thread(&FaceRecognition::ReadSubProcessLogs, this);
    Sleep(1500);
    return true;
}

bool FaceRecognition::start_fr_capture_process(const std::string& map_name, std::string& error_message) {
    char exe_path[MAX_PATH];
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    fs::path fr_exe = fs::path(exe_path).parent_path() / "FaceRecognizer.exe";
    if (!fs::exists(fr_exe)) {
        error_message = "FaceRecognizer.exe 不存在";
        return false;
    }
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string cmd_line = "\"" + fr_exe.string() + "\" -m capture-image --frame-map \"" + map_name + "\"";
    if (!CreateProcessA(nullptr, cmd_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        error_message = "启动 FaceRecognizer 抓拍进程失败";
        return false;
    }
    DWORD wait_result = WaitForSingleObject(pi.hProcess, 15000);
    if (wait_result == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        error_message = "等待 FaceRecognizer 抓拍超时";
        return false;
    }
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (exit_code != 0) {
        error_message = "FaceRecognizer 抓拍进程返回失败";
        return false;
    }
    return true;
}

void FaceRecognition::ReadSubProcessLogs() {
    char buffer[4096];
    DWORD bytesRead;
    while (log_thread_running_) {
        DWORD bytesAvail = 0;
        if (PeekNamedPipe(fr_stdout_read_, NULL, 0, NULL, &bytesAvail, NULL) && bytesAvail > 0) {
            if (ReadFile(fr_stdout_read_, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
                buffer[bytesRead] = '\0';
                std::cout << "[FR OUT]: " << buffer;
            }
        }
        DWORD errAvail = 0;
        if (PeekNamedPipe(fr_stderr_read_, NULL, 0, NULL, &errAvail, NULL) && errAvail > 0) {
            if (ReadFile(fr_stderr_read_, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
                buffer[bytesRead] = '\0';
                std::cerr << "[FR ERR]: " << buffer;
            }
        }
        DWORD exitCode = 0;
        if (fr_process_ && GetExitCodeProcess(fr_process_, &exitCode) && exitCode != STILL_ACTIVE) break;
        Sleep(50);
    }
}

void FaceRecognition::stop_fr_process() {
    if (fr_process_) {
        if (udp_sender_) udp_sender_->send_status(RecognitionStatus::PROCESS_ENDED, "");
        if (WaitForSingleObject(fr_process_, 3000) == WAIT_TIMEOUT) TerminateProcess(fr_process_, 0);
        CloseHandle(fr_process_info_.hProcess);
        CloseHandle(fr_process_info_.hThread);
        fr_process_ = nullptr;
    }
    log_thread_running_ = false;
    if (log_thread_.joinable()) log_thread_.join();
    if (fr_stdout_read_) { CloseHandle(fr_stdout_read_); fr_stdout_read_ = nullptr; }
    if (fr_stderr_read_) { CloseHandle(fr_stderr_read_); fr_stderr_read_ = nullptr; }
}

void FaceRecognition::on_fr_status_received(RecognitionStatus status, const std::string& username) {
    last_result_.success = (status == RecognitionStatus::SUCCESS);
    last_result_.username = username.empty() ? pending_username_hint_ : username;
    last_result_.confidence = 0.0f;
    last_result_.error_message = (status == RecognitionStatus::SUCCESS) ? "" : "Recognition failed";
    if (udp_sender_) udp_sender_->send_status(status, last_result_.username);
}

bool FaceRecognition::Initialize(std::string& error_message) {
    if (initialized_) return true;
    try {
        udp_receiver_cp_ = std::make_unique<UdpReceiverFromCP>(51236);
        udp_receiver_cp_->set_callback([this](AuthRequestType type, const std::string& username_hint, uint32_t session_id) {
            on_cp_request_received(type, username_hint, session_id);
        });
        udp_receiver_cp_->start();

        udp_receiver_fr_ = std::make_unique<UdpReceiverFromFR>(51235);
        udp_receiver_fr_->set_callback([this](RecognitionStatus status, const std::string& username) {
            on_fr_status_received(status, username);
        });
        udp_receiver_fr_->start();

        udp_sender_ = std::make_unique<UdpSenderToCP>("127.0.0.1", 51234);
        initialized_ = true;
        error_message = "人脸识别模块已初始化";
        return true;
    } catch (const std::exception& e) {
        error_message = std::string("初始化失败: ") + e.what();
        if (udp_receiver_cp_) udp_receiver_cp_->stop();
        if (udp_receiver_fr_) udp_receiver_fr_->stop();
        return false;
    }
}

bool FaceRecognition::StartRecognition() {
    if (!initialized_) return false;
    if (!fr_process_ && !start_fr_process()) return false;
    is_running_ = true;
    return true;
}

bool FaceRecognition::CaptureFrameImage(std::vector<unsigned char>& image_data, int& width, int& height, std::string& error_message) {
    width = 0;
    height = 0;
    image_data.clear();
    const std::string map_name = "Local\\Smile2Unlock_Frame_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64());
    HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kSharedFrameMappingSize, map_name.c_str());
    if (!mapping) {
        error_message = "创建共享内存失败";
        return false;
    }
    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, kSharedFrameMappingSize);
    if (!view) {
        CloseHandle(mapping);
        error_message = "映射共享内存失败";
        return false;
    }
    auto* header = static_cast<SharedFrameHeader*>(view);
    *header = SharedFrameHeader{};
    bool success = false;
    if (start_fr_capture_process(map_name, error_message)) {
        if (header->magic == SharedFrameHeader::MAGIC &&
            header->version == SharedFrameHeader::VERSION &&
            header->status_code == static_cast<int32_t>(SharedFrameStatus::READY) &&
            header->image_bytes > 0 &&
            header->image_bytes <= SharedFrameHeader::MAX_IMAGE_BYTES) {
            width = header->width;
            height = header->height;
            auto* bytes = reinterpret_cast<unsigned char*>(header + 1);
            image_data.assign(bytes, bytes + header->image_bytes);
            success = true;
        } else {
            error_message = "共享图像数据无效";
        }
    }
    UnmapViewOfFile(view);
    CloseHandle(mapping);
    return success;
}

void FaceRecognition::StopRecognition() {
    if (!is_running_) return;
    is_running_ = false;
    stop_fr_process();
}

RecognitionResult FaceRecognition::GetLastResult() const { return last_result_; }

std::string FaceRecognition::ExtractFaceFeature(const std::string&) { return ""; }

bool FaceRecognition::CompareFaces(const std::string& feature1, const std::string& feature2, float& similarity) {
    if (!initialized_ || feature1.empty() || feature2.empty()) {
        similarity = 0.0f;
        return false;
    }
    similarity = 0.0f;
    return false;
}

void FaceRecognition::on_cp_request_received(AuthRequestType type, const std::string& username_hint, uint32_t session_id) {
    current_session_id_ = session_id;
    pending_username_hint_ = username_hint;
    switch (type) {
        case AuthRequestType::START_RECOGNITION:
            if (!fr_process_ && !start_fr_process()) {
                if (udp_sender_) udp_sender_->send_status(RecognitionStatus::RECOGNITION_ERROR, "");
                return;
            }
            is_running_ = true;
            if (udp_sender_) udp_sender_->send_status(RecognitionStatus::RECOGNIZING, "");
            break;
        case AuthRequestType::CANCEL_RECOGNITION:
            StopRecognition();
            if (udp_sender_) udp_sender_->send_status(RecognitionStatus::IDLE, "");
            break;
        case AuthRequestType::QUERY_STATUS:
            if (udp_sender_) {
                RecognitionStatus current_status = is_running_ ? RecognitionStatus::RECOGNIZING : RecognitionStatus::IDLE;
                udp_sender_->send_status(current_status, last_result_.username);
            }
            break;
        default:
            break;
    }
}

} // namespace smile2unlock::managers
