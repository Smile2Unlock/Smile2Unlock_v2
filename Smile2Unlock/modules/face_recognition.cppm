module;

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#include <boost/asio.hpp>

// 使用统一的 UDP 管理头文件
#include "managers/ipc/udp/udp_manager.h"
#include "models/shared_frame_ipc.h"

export module smile2unlock.face_recognition;

import std;
import service_runtime;
import smile2unlock.models;
import smile2unlock.database;

namespace fs = std::filesystem;

namespace smile2unlock::managers {
namespace {
constexpr DWORD kSharedFrameMappingSize =
    sizeof(smile2unlock::SharedFrameHeader) +
    smile2unlock::SharedFrameHeader::MAX_IMAGE_BYTES +
    smile2unlock::SharedFrameHeader::MAX_FEATURE_BYTES;

const char* ToString(RecognitionStatus status) {
    switch (status) {
        case RecognitionStatus::IDLE: return "IDLE";
        case RecognitionStatus::RECOGNIZING: return "RECOGNIZING";
        case RecognitionStatus::SUCCESS: return "SUCCESS";
        case RecognitionStatus::FAILED: return "FAILED";
        case RecognitionStatus::TIMEOUT: return "TIMEOUT";
        case RecognitionStatus::RECOGNITION_ERROR: return "RECOGNITION_ERROR";
        case RecognitionStatus::FACE_DETECTED: return "FACE_DETECTED";
        case RecognitionStatus::PROCESS_ENDED: return "PROCESS_ENDED";
        default: return "UNKNOWN";
    }
}

const char* ToString(AuthRequestType type) {
    switch (type) {
        case AuthRequestType::START_RECOGNITION: return "START_RECOGNITION";
        case AuthRequestType::CANCEL_RECOGNITION: return "CANCEL_RECOGNITION";
        case AuthRequestType::QUERY_STATUS: return "QUERY_STATUS";
        case AuthRequestType::GET_PASSWORD: return "GET_PASSWORD";
        default: return "UNKNOWN";
    }
}

bool IsProcessHandleActive(HANDLE process_handle) {
    if (!process_handle) {
        return false;
    }
    DWORD exit_code = 0;
    return GetExitCodeProcess(process_handle, &exit_code) && exit_code == STILL_ACTIVE;
}

bool BuildSharedMappingSecurity(SECURITY_ATTRIBUTES& sa, SECURITY_DESCRIPTOR& sd) {
    if (!InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION)) {
        return false;
    }
    if (!SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE)) {
        return false;
    }
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle = FALSE;
    return true;
}

std::string LastErrorText(const char* prefix) {
    std::ostringstream ss;
    ss << prefix << " (GetLastError=" << GetLastError() << ")";
    return ss.str();
}

// 使用统一 UDP 类的别名
using UdpReceiverFromFR = ::smile2unlock::udp::StatusReceiver;
using UdpSenderToCP = ::smile2unlock::udp::StatusSender;
using UdpPasswordSenderToCP = ::smile2unlock::udp::PasswordSender;
using UdpReceiverFromCP = ::smile2unlock::udp::AuthRequestReceiver;

// 端口常量
constexpr uint16_t kFrStatusPort = ::smile2unlock::udp::UdpPorts::kFrStatusPort;
constexpr uint16_t kCpStatusPort = ::smile2unlock::udp::UdpPorts::kCpStatusPort;
constexpr uint16_t kAuthRequestPort = ::smile2unlock::udp::UdpPorts::kAuthRequestPort;
constexpr uint16_t kPasswordPort = ::smile2unlock::udp::UdpPorts::kPasswordPort;

} // anonymous namespace
} // namespace smile2unlock::managers

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
    bool StartPreviewStream(std::string& error_message);
    void StopPreviewStream();
    bool IsPreviewStreamRunning() const;
    bool GetLatestPreviewFrame(std::vector<unsigned char>& image_data, int& width, int& height, std::string& error_message);
    bool CaptureFrameImage(std::vector<unsigned char>& image_data, int& width, int& height, std::string& error_message);
    bool CaptureFrameAndFeature(std::vector<unsigned char>& image_data, int& width, int& height, std::string& feature, std::string& error_message);

    RecognitionResult GetLastResult() const;
    std::string ExtractFaceFeature(const std::string& image_path);
    bool CompareFaces(const std::string& feature1, const std::string& feature2, float& similarity);
    void SetConfig(const FaceRecognizerConfig& config) { config_ = config; }
    FaceRecognizerConfig GetConfig() const { return config_; }

private:
    static constexpr float kRecognitionThreshold = 0.62f;

    bool initialized_;
    bool is_running_;
    RecognitionResult last_result_;

    HANDLE fr_process_;
    PROCESS_INFORMATION fr_process_info_;
    HANDLE fr_stdout_read_;
    HANDLE fr_stderr_read_;
    HANDLE recognition_result_mapping_;
    void* recognition_result_view_;
    std::string recognition_result_map_name_;
    std::thread log_thread_;
    bool log_thread_running_;
    HANDLE preview_process_;
    PROCESS_INFORMATION preview_process_info_;
    HANDLE preview_mapping_;
    void* preview_view_;
    std::string preview_map_name_;
    uint32_t last_preview_sequence_;

    void ReadSubProcessLogs();

    std::unique_ptr<UdpReceiverFromCP> udp_receiver_cp_;
    std::unique_ptr<UdpReceiverFromFR> udp_receiver_fr_;
    std::unique_ptr<UdpSenderToCP> udp_sender_;
    std::unique_ptr<UdpPasswordSenderToCP> udp_password_sender_;
    std::unique_ptr<smile2unlock::managers::Database> database_;
    FaceRecognizerConfig config_;

    uint32_t current_session_id_;
    std::string pending_username_hint_;

    bool start_fr_process();
    bool start_fr_capture_process(const std::string& map_name, bool extract_feature, std::string& error_message);
    bool start_fr_preview_process(const std::string& map_name, std::string& error_message);
    void stop_fr_process();
    void cleanup_fr_process_handles();
    void on_cp_request_received(AuthRequestType type, const std::string& username_hint, uint32_t session_id);
    void on_fr_status_received(RecognitionStatus status, const std::string& username);
    bool capture_shared_payload(std::vector<unsigned char>& image_data, int& width, int& height, std::string* feature, std::string& error_message);
    bool compare_features_with_fr(const std::string& feature1, const std::string& feature2, float& similarity);
    std::string resolve_recognized_username(std::string& error_message);
    std::string read_recognition_feature(std::string& error_message) const;
};

} // namespace smile2unlock::managers

module :private;

namespace smile2unlock::managers {

FaceRecognition::FaceRecognition()
    : initialized_(false), is_running_(false), fr_process_(nullptr), current_session_id_(0),
      fr_stdout_read_(nullptr), fr_stderr_read_(nullptr), recognition_result_mapping_(nullptr),
      recognition_result_view_(nullptr), log_thread_running_(false),
      preview_process_(nullptr), preview_mapping_(nullptr), preview_view_(nullptr), last_preview_sequence_(0) {}

FaceRecognition::~FaceRecognition() {
    StopPreviewStream();
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

    SECURITY_ATTRIBUTES mapping_sa{};
    SECURITY_DESCRIPTOR mapping_sd{};
    if (!BuildSharedMappingSecurity(mapping_sa, mapping_sd)) {
        CloseHandle(hStdoutWrite);
        CloseHandle(hStderrWrite);
        return false;
    }

    recognition_result_map_name_ = "Local\\Smile2Unlock_Recognition_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64());
    recognition_result_mapping_ = CreateFileMappingA(INVALID_HANDLE_VALUE, &mapping_sa, PAGE_READWRITE, 0, kSharedFrameMappingSize, recognition_result_map_name_.c_str());
    if (!recognition_result_mapping_) {
        return false;
    }
    recognition_result_view_ = MapViewOfFile(recognition_result_mapping_, FILE_MAP_ALL_ACCESS, 0, 0, kSharedFrameMappingSize);
    if (!recognition_result_view_) {
        CloseHandle(recognition_result_mapping_);
        recognition_result_mapping_ = nullptr;
        recognition_result_map_name_.clear();
        return false;
    }
    auto* result_header = static_cast<SharedFrameHeader*>(recognition_result_view_);
    *result_header = SharedFrameHeader{};

    std::ostringstream cmd;
    cmd << "\"" << fr_exe.string() << "\" -m recognize"
        << " --udp-port " << kFrStatusPort
        << " --camera " << config_.camera
        << " --liveness-detection " << (config_.liveness ? "true" : "false")
        << " --threshold face=" << config_.face_threshold
        << " --threshold liveness=" << config_.liveness_threshold
        << " --debug " << (config_.debug ? "true" : "false")
        << " --result-map \"" << recognition_result_map_name_ << "\"";
    std::string cmd_line = cmd.str();
    if (!CreateProcessA(nullptr, cmd_line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &fr_process_info_)) {
        UnmapViewOfFile(recognition_result_view_);
        CloseHandle(recognition_result_mapping_);
        recognition_result_view_ = nullptr;
        recognition_result_mapping_ = nullptr;
        recognition_result_map_name_.clear();
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

bool FaceRecognition::start_fr_capture_process(const std::string& map_name, bool extract_feature, std::string& error_message) {
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
    std::ostringstream cmd;
    cmd << "\"" << fr_exe.string() << "\" -m capture-image"
        << " --camera " << config_.camera
        << " --liveness-detection " << (config_.liveness ? "true" : "false")
        << " --threshold face=" << config_.face_threshold
        << " --threshold liveness=" << config_.liveness_threshold
        << " --debug " << (config_.debug ? "true" : "false")
        << " --frame-map \"" << map_name << "\"";
    std::string cmd_line = cmd.str();
    if (extract_feature) {
        cmd_line += " --extract-feature true";
    }
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

bool FaceRecognition::start_fr_preview_process(const std::string& map_name, std::string& error_message) {
    char exe_path[MAX_PATH];
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    fs::path fr_exe = fs::path(exe_path).parent_path() / "FaceRecognizer.exe";
    if (!fs::exists(fr_exe)) {
        error_message = "FaceRecognizer.exe 不存在";
        return false;
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    memset(&preview_process_info_, 0, sizeof(preview_process_info_));
    std::ostringstream cmd;
    cmd << "\"" << fr_exe.string() << "\" -m preview-stream"
        << " --camera " << config_.camera
        << " --liveness-detection " << (config_.liveness ? "true" : "false")
        << " --threshold face=" << config_.face_threshold
        << " --threshold liveness=" << config_.liveness_threshold
        << " --debug " << (config_.debug ? "true" : "false")
        << " --frame-map \"" << map_name << "\"";
    std::string cmd_line = cmd.str();
    if (!CreateProcessA(nullptr, cmd_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &preview_process_info_)) {
        error_message = "启动 FaceRecognizer 预览进程失败";
        return false;
    }

    preview_process_ = preview_process_info_.hProcess;
    Sleep(300);
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
    if (recognition_result_view_ != nullptr) {
        auto* header = static_cast<SharedFrameHeader*>(recognition_result_view_);
        header->stop_requested = 1;
    }

    if (fr_process_) {
        std::cout << "[SU] stop_fr_process session=" << current_session_id_
                  << " notifying PROCESS_ENDED" << std::endl;
        if (udp_sender_) udp_sender_->send_status(RecognitionStatus::PROCESS_ENDED, "", current_session_id_);
        if (IsProcessHandleActive(fr_process_) && WaitForSingleObject(fr_process_, 500) == WAIT_TIMEOUT) {
            TerminateProcess(fr_process_, 0);
        }
    }
    cleanup_fr_process_handles();
}

void FaceRecognition::cleanup_fr_process_handles() {
    log_thread_running_ = false;
    if (log_thread_.joinable()) log_thread_.join();
    if (fr_process_info_.hProcess) {
        CloseHandle(fr_process_info_.hProcess);
        fr_process_info_.hProcess = nullptr;
    }
    if (fr_process_info_.hThread) {
        CloseHandle(fr_process_info_.hThread);
        fr_process_info_.hThread = nullptr;
    }
    fr_process_ = nullptr;
    if (recognition_result_view_) {
        UnmapViewOfFile(recognition_result_view_);
        recognition_result_view_ = nullptr;
    }
    if (recognition_result_mapping_) {
        CloseHandle(recognition_result_mapping_);
        recognition_result_mapping_ = nullptr;
    }
    recognition_result_map_name_.clear();
    if (fr_stdout_read_) { CloseHandle(fr_stdout_read_); fr_stdout_read_ = nullptr; }
    if (fr_stderr_read_) { CloseHandle(fr_stderr_read_); fr_stderr_read_ = nullptr; }
}

void FaceRecognition::on_fr_status_received(RecognitionStatus status, const std::string& username) {
    NotifyServiceActivity();

    std::cout << "[SU] FR状态回调"
              << " session=" << current_session_id_
              << " status=" << ToString(status)
              << "(" << static_cast<int>(status) << ")"
              << " username=" << (username.empty() ? "(empty)" : username)
              << " pending_hint=" << (pending_username_hint_.empty() ? "(empty)" : pending_username_hint_)
              << std::endl;

    RecognitionStatus final_status = status;
    last_result_.success = (status == RecognitionStatus::SUCCESS);
    last_result_.username = username.empty() ? pending_username_hint_ : username;
    last_result_.confidence = 0.0f;
    last_result_.error_message = (status == RecognitionStatus::SUCCESS) ? "" : "Recognition failed";

    if (status == RecognitionStatus::SUCCESS && last_result_.username.empty()) {
        std::string match_error;
        const std::string matched_username = resolve_recognized_username(match_error);
        if (!matched_username.empty()) {
            last_result_.username = matched_username;
            std::cout << "[SU] 人脸匹配成功"
                      << " session=" << current_session_id_
                      << " username=" << last_result_.username
                      << " similarity=" << last_result_.confidence
                      << std::endl;
        } else if (!match_error.empty()) {
            final_status = RecognitionStatus::RECOGNITION_ERROR;
            last_result_.success = false;
            last_result_.error_message = match_error;
            std::cout << "[SU] 人脸匹配失败"
                      << " session=" << current_session_id_
                      << " error=" << match_error
                      << std::endl;
        }
    }

    if (final_status == RecognitionStatus::SUCCESS ||
        final_status == RecognitionStatus::TIMEOUT ||
        final_status == RecognitionStatus::RECOGNITION_ERROR ||
        final_status == RecognitionStatus::PROCESS_ENDED) {
        is_running_ = false;
    }
    std::cout << "[SU] 状态归并完成"
              << " session=" << current_session_id_
              << " final_status=" << ToString(final_status)
              << "(" << static_cast<int>(final_status) << ")"
              << " success=" << (last_result_.success ? 1 : 0)
              << " username=" << (last_result_.username.empty() ? "(empty)" : last_result_.username)
              << " is_running=" << (is_running_ ? 1 : 0)
              << std::endl;
    if (udp_sender_) udp_sender_->send_status(final_status, last_result_.username, current_session_id_);
}

bool FaceRecognition::Initialize(std::string& error_message) {
    if (initialized_) return true;
    try {
        NotifyServiceActivity();

        database_ = std::make_unique<smile2unlock::managers::Database>();
        if (!database_->Initialize()) {
            error_message = "初始化识别数据库失败";
            database_.reset();
            return false;
        }

        udp_receiver_cp_ = std::make_unique<UdpReceiverFromCP>(kAuthRequestPort);
        udp_receiver_cp_->set_callback([this](AuthRequestType type, const std::string& username_hint, uint32_t session_id) {
            on_cp_request_received(type, username_hint, session_id);
        });
        udp_receiver_cp_->start();

        udp_receiver_fr_ = std::make_unique<UdpReceiverFromFR>(kFrStatusPort);
        udp_receiver_fr_->set_callback([this](RecognitionStatus status, const std::string& username) {
            on_fr_status_received(status, username);
        });
        udp_receiver_fr_->start();

        udp_sender_ = std::make_unique<UdpSenderToCP>("127.0.0.1", kCpStatusPort);
        udp_password_sender_ = std::make_unique<UdpPasswordSenderToCP>("127.0.0.1", kPasswordPort);
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
    if (fr_process_ && !IsProcessHandleActive(fr_process_)) {
        cleanup_fr_process_handles();
    }
    if (!fr_process_ && !start_fr_process()) return false;
    is_running_ = true;
    return true;
}

bool FaceRecognition::CaptureFrameImage(std::vector<unsigned char>& image_data, int& width, int& height, std::string& error_message) {
    return capture_shared_payload(image_data, width, height, nullptr, error_message);
}

bool FaceRecognition::StartPreviewStream(std::string& error_message) {
    if (IsPreviewStreamRunning() && preview_view_ && preview_mapping_) {
        return true;
    }
    if (preview_process_ || preview_view_ || preview_mapping_) {
        StopPreviewStream();
    }

    SECURITY_ATTRIBUTES mapping_sa{};
    SECURITY_DESCRIPTOR mapping_sd{};
    if (!BuildSharedMappingSecurity(mapping_sa, mapping_sd)) {
        error_message = LastErrorText("构建预览共享内存安全描述符失败");
        return false;
    }

    preview_map_name_ = "Local\\Smile2Unlock_Preview_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64());
    preview_mapping_ = CreateFileMappingA(INVALID_HANDLE_VALUE, &mapping_sa, PAGE_READWRITE, 0, kSharedFrameMappingSize, preview_map_name_.c_str());
    if (!preview_mapping_) {
        error_message = LastErrorText("创建预览共享内存失败");
        return false;
    }

    preview_view_ = MapViewOfFile(preview_mapping_, FILE_MAP_ALL_ACCESS, 0, 0, kSharedFrameMappingSize);
    if (!preview_view_) {
        CloseHandle(preview_mapping_);
        preview_mapping_ = nullptr;
        error_message = LastErrorText("映射预览共享内存失败");
        return false;
    }

    auto* header = static_cast<SharedFrameHeader*>(preview_view_);
    *header = SharedFrameHeader{};
    last_preview_sequence_ = 0;

    if (!start_fr_preview_process(preview_map_name_, error_message)) {
        UnmapViewOfFile(preview_view_);
        CloseHandle(preview_mapping_);
        preview_view_ = nullptr;
        preview_mapping_ = nullptr;
        preview_map_name_.clear();
        return false;
    }

    error_message = "摄像头已打开";
    return true;
}

void FaceRecognition::StopPreviewStream() {
    if (preview_view_ != nullptr) {
        auto* header = static_cast<SharedFrameHeader*>(preview_view_);
        header->stop_requested = 1;
    }

    if (preview_process_) {
        if (WaitForSingleObject(preview_process_, 2000) == WAIT_TIMEOUT) {
            TerminateProcess(preview_process_, 0);
        }
        CloseHandle(preview_process_info_.hProcess);
        CloseHandle(preview_process_info_.hThread);
        preview_process_ = nullptr;
        memset(&preview_process_info_, 0, sizeof(preview_process_info_));
    }

    if (preview_view_) {
        UnmapViewOfFile(preview_view_);
        preview_view_ = nullptr;
    }
    if (preview_mapping_) {
        CloseHandle(preview_mapping_);
        preview_mapping_ = nullptr;
    }
    preview_map_name_.clear();
    last_preview_sequence_ = 0;
}

bool FaceRecognition::IsPreviewStreamRunning() const {
    if (!preview_process_) {
        return false;
    }
    DWORD exit_code = 0;
    return GetExitCodeProcess(preview_process_, &exit_code) && exit_code == STILL_ACTIVE;
}

bool FaceRecognition::GetLatestPreviewFrame(std::vector<unsigned char>& image_data, int& width, int& height, std::string& error_message) {
    image_data.clear();
    width = 0;
    height = 0;

    if (!IsPreviewStreamRunning() || preview_view_ == nullptr) {
        error_message = "摄像头未开启";
        return false;
    }

    auto* header = static_cast<SharedFrameHeader*>(preview_view_);
    if (header->magic != SharedFrameHeader::MAGIC || header->version != SharedFrameHeader::VERSION) {
        error_message = "预览共享内存无效";
        return false;
    }
    if (header->status_code != static_cast<int32_t>(SharedFrameStatus::READY)) {
        error_message = "摄像头尚未输出可用画面";
        return false;
    }
    if (header->image_bytes == 0 || header->image_bytes > SharedFrameHeader::MAX_IMAGE_BYTES) {
        error_message = "预览帧数据无效";
        return false;
    }
    if (header->frame_sequence == last_preview_sequence_) {
        return false;
    }

    auto* bytes = reinterpret_cast<unsigned char*>(header + 1);
    image_data.assign(bytes, bytes + header->image_bytes);
    width = header->width;
    height = header->height;
    last_preview_sequence_ = header->frame_sequence;
    return true;
}

bool FaceRecognition::CaptureFrameAndFeature(std::vector<unsigned char>& image_data, int& width, int& height, std::string& feature, std::string& error_message) {
    feature.clear();
    return capture_shared_payload(image_data, width, height, &feature, error_message);
}

bool FaceRecognition::capture_shared_payload(std::vector<unsigned char>& image_data, int& width, int& height, std::string* feature, std::string& error_message) {
    width = 0;
    height = 0;
    image_data.clear();
    if (feature != nullptr) {
        feature->clear();
    }
    SECURITY_ATTRIBUTES mapping_sa{};
    SECURITY_DESCRIPTOR mapping_sd{};
    if (!BuildSharedMappingSecurity(mapping_sa, mapping_sd)) {
        error_message = LastErrorText("构建抓拍共享内存安全描述符失败");
        return false;
    }
    const std::string map_name = "Local\\Smile2Unlock_Frame_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64());
    HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, &mapping_sa, PAGE_READWRITE, 0, kSharedFrameMappingSize, map_name.c_str());
    if (!mapping) {
        error_message = LastErrorText("创建共享内存失败");
        return false;
    }
    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, kSharedFrameMappingSize);
    if (!view) {
        CloseHandle(mapping);
        error_message = LastErrorText("映射共享内存失败");
        return false;
    }
    auto* header = static_cast<SharedFrameHeader*>(view);
    *header = SharedFrameHeader{};
    bool success = false;
    if (start_fr_capture_process(map_name, feature != nullptr, error_message)) {
        if (header->magic == SharedFrameHeader::MAGIC &&
            header->version == SharedFrameHeader::VERSION &&
            header->image_bytes > 0 &&
            header->image_bytes <= SharedFrameHeader::MAX_IMAGE_BYTES) {
            width = header->width;
            height = header->height;
            auto* bytes = reinterpret_cast<unsigned char*>(header + 1);
            image_data.assign(bytes, bytes + header->image_bytes);

            if (header->status_code == static_cast<int32_t>(SharedFrameStatus::READY)) {
                if (feature != nullptr) {
                    if (header->feature_bytes == 0 || header->feature_bytes > SharedFrameHeader::MAX_FEATURE_BYTES) {
                        error_message = "共享特征数据无效";
                    } else {
                        const char* feature_bytes = reinterpret_cast<const char*>(bytes + header->image_bytes);
                        feature->assign(feature_bytes, feature_bytes + header->feature_bytes);
                        success = true;
                    }
                } else {
                    success = true;
                }
            } else if (header->status_code == static_cast<int32_t>(SharedFrameStatus::FEATURE_EXTRACTION_FAILED)) {
                error_message = "未检测到可录入的人脸，请调整角度后重试";
            } else if (header->status_code == static_cast<int32_t>(SharedFrameStatus::FEATURE_TOO_LARGE)) {
                error_message = "人脸特征数据过大，无法保存";
            } else if (header->status_code == static_cast<int32_t>(SharedFrameStatus::INVALID_FRAME)) {
                error_message = "共享图像数据无效";
            } else if (header->status_code == static_cast<int32_t>(SharedFrameStatus::CAPTURE_FAILED)) {
                error_message = "FaceRecognizer 抓拍失败";
            } else {
                error_message = "共享图像数据无效";
            }
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
    return compare_features_with_fr(feature1, feature2, similarity);
}

bool FaceRecognition::compare_features_with_fr(const std::string& feature1, const std::string& feature2, float& similarity) {
    similarity = 0.0f;

    char exe_path[MAX_PATH];
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    fs::path fr_exe = fs::path(exe_path).parent_path() / "FaceRecognizer.exe";
    if (!fs::exists(fr_exe)) {
        return false;
    }

    SECURITY_ATTRIBUTES saAttr{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    if (!CreatePipe(&stdout_read, &stdout_write, &saAttr, 0)) {
        return false;
    }
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.hStdOutput = stdout_write;
    si.hStdError = stdout_write;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi{};
    std::string cmd_line = "\"" + fr_exe.string() + "\" -m compare-features --feature-a \"" + feature1 + "\" --feature-b \"" + feature2 + "\"";
    const BOOL created = CreateProcessA(nullptr, cmd_line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(stdout_write);
    if (!created) {
        CloseHandle(stdout_read);
        return false;
    }

    std::string output;
    char buffer[512];
    DWORD bytes_read = 0;
    while (ReadFile(stdout_read, buffer, sizeof(buffer) - 1, &bytes_read, nullptr) && bytes_read > 0) {
        buffer[bytes_read] = '\0';
        output.append(buffer, bytes_read);
    }

    WaitForSingleObject(pi.hProcess, 10000);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(stdout_read);

    if (exit_code != 0) {
        return false;
    }

    std::regex float_pattern(R"(([0-9]+(?:\.[0-9]+)?))");
    std::sregex_iterator begin(output.begin(), output.end(), float_pattern);
    std::sregex_iterator end;
    if (begin == end) {
        return false;
    }
    std::string last_match;
    for (auto it = begin; it != end; ++it) {
        last_match = (*it)[1].str();
    }

    try {
        similarity = std::stof(last_match);
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

std::string FaceRecognition::resolve_recognized_username(std::string& error_message) {
    error_message.clear();
    if (!database_) {
        error_message = "识别数据库未初始化";
        return "";
    }

    const std::string probe_feature = read_recognition_feature(error_message);
    if (probe_feature.empty()) {
        return "";
    }

    const auto users = database_->GetAllUsers();
    float best_similarity = 0.0f;
    std::string best_username;

    for (const auto& user : users) {
        for (const auto& face : user.faces) {
            if (face.feature.empty()) {
                continue;
            }

            float similarity = 0.0f;
            if (!compare_features_with_fr(probe_feature, face.feature, similarity)) {
                continue;
            }

            if (similarity > best_similarity) {
                best_similarity = similarity;
                best_username = user.username;
            }
        }
    }

    if (best_username.empty()) {
        error_message = "未找到匹配的人脸用户";
        return "";
    }
    if (best_similarity < kRecognitionThreshold) {
        error_message = "匹配分数不足，拒绝识别";
        return "";
    }

    last_result_.confidence = best_similarity;
    return best_username;
}

std::string FaceRecognition::read_recognition_feature(std::string& error_message) const {
    error_message.clear();
    if (recognition_result_view_ == nullptr) {
        error_message = "识别结果共享内存不存在";
        return "";
    }

    const auto* header = static_cast<const SharedFrameHeader*>(recognition_result_view_);
    if (header->magic != SharedFrameHeader::MAGIC || header->version != SharedFrameHeader::VERSION) {
        error_message = "识别结果共享内存无效";
        return "";
    }
    if (header->status_code != static_cast<int32_t>(SharedFrameStatus::READY)) {
        error_message = "识别结果特征尚未就绪";
        return "";
    }
    if (header->feature_bytes == 0 || header->feature_bytes > SharedFrameHeader::MAX_FEATURE_BYTES) {
        error_message = "识别结果特征无效";
        return "";
    }

    const char* feature_bytes = reinterpret_cast<const char*>(header + 1);
    return std::string(feature_bytes, feature_bytes + header->feature_bytes);
}

void FaceRecognition::on_cp_request_received(AuthRequestType type, const std::string& username_hint, uint32_t session_id) {
    NotifyServiceActivity();

    current_session_id_ = session_id;
    pending_username_hint_ = username_hint;
    std::cout << "[CP->SU] 收到请求"
              << " session=" << session_id
              << " type=" << ToString(type)
              << " username_hint=" << (username_hint.empty() ? "(empty)" : username_hint)
              << " is_running=" << (is_running_ ? 1 : 0)
              << " fr_process_alive=" << ((fr_process_ && IsProcessHandleActive(fr_process_)) ? 1 : 0)
              << std::endl;
    switch (type) {
        case AuthRequestType::START_RECOGNITION:
            if (fr_process_ && !IsProcessHandleActive(fr_process_)) {
                std::cout << "[SU] START_RECOGNITION 检测到旧 FR 进程已退出，执行清理"
                          << " session=" << session_id << std::endl;
                cleanup_fr_process_handles();
            }
            if (!fr_process_ && !start_fr_process()) {
                std::cout << "[SU] START_RECOGNITION 启动 FR 进程失败"
                          << " session=" << session_id << std::endl;
                if (udp_sender_) udp_sender_->send_status(RecognitionStatus::RECOGNITION_ERROR, "", session_id);
                return;
            }
            is_running_ = true;
            std::cout << "[SU] START_RECOGNITION 已进入识别中"
                      << " session=" << session_id << std::endl;
            if (udp_sender_) udp_sender_->send_status(RecognitionStatus::RECOGNIZING, "", session_id);
            break;
        case AuthRequestType::CANCEL_RECOGNITION:
            std::cout << "[SU] CANCEL_RECOGNITION 开始停止识别"
                      << " session=" << session_id << std::endl;
            StopRecognition();
            if (udp_sender_) udp_sender_->send_status(RecognitionStatus::IDLE, "", session_id);
            break;
        case AuthRequestType::QUERY_STATUS:
            if (udp_sender_) {
                if (fr_process_ && !IsProcessHandleActive(fr_process_)) {
                    std::cout << "[SU] QUERY_STATUS 检测到 FR 进程已退出，更新为未运行"
                              << " session=" << session_id << std::endl;
                    cleanup_fr_process_handles();
                    is_running_ = false;
                }
                RecognitionStatus current_status = is_running_ ? RecognitionStatus::RECOGNIZING : RecognitionStatus::IDLE;
                std::cout << "[SU] QUERY_STATUS 返回当前状态"
                          << " session=" << session_id
                          << " current_status=" << ToString(current_status)
                          << " last_username=" << (last_result_.username.empty() ? "(empty)" : last_result_.username)
                          << std::endl;
                udp_sender_->send_status(current_status, last_result_.username, session_id);
            }
            break;
        case AuthRequestType::GET_PASSWORD:
            if (!udp_password_sender_ || !database_) {
                std::cout << "[SU] GET_PASSWORD 依赖未初始化"
                          << " session=" << session_id << std::endl;
                break;
            }
            if (const auto user = database_->GetUserByUsername(username_hint); user.has_value()) {
                const std::string decrypted_password = database_->DecryptPassword(user->encrypted_password);
                std::cout << "[SU] GET_PASSWORD 查库完成"
                          << " session=" << session_id
                          << " lookup_username=" << username_hint
                          << " resolved_username=" << user->username
                          << " password_found=" << (!decrypted_password.empty() ? 1 : 0)
                          << std::endl;
                udp_password_sender_->send_password(session_id, user->username, decrypted_password, !decrypted_password.empty());
            } else {
                std::cout << "[SU] GET_PASSWORD 未找到用户"
                          << " session=" << session_id
                          << " lookup_username=" << (username_hint.empty() ? "(empty)" : username_hint)
                          << std::endl;
                udp_password_sender_->send_password(session_id, username_hint, "", false);
            }
            break;
        default:
            break;
    }
}

} // namespace smile2unlock::managers
