#include "Smile2Unlock/backend/managers/face_recognition.h"
#include <filesystem>
#include <iostream>
#include <windows.h>

namespace fs = std::filesystem;

namespace smile2unlock {
namespace managers {

FaceRecognition::FaceRecognition() 
    : initialized_(false), is_running_(false), fr_process_(nullptr), current_session_id_(0)
    , fr_stdout_read_(nullptr)
    , fr_stderr_read_(nullptr)
    , log_thread_running_(false) {
}

FaceRecognition::~FaceRecognition() {
    StopRecognition();
    stop_fr_process();
}

bool FaceRecognition::start_fr_process() {
    // 获取 FaceRecognizer.exe 路径
    char exe_path[MAX_PATH];
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    fs::path exe_dir = fs::path(exe_path).parent_path();
    fs::path fr_exe = exe_dir / "FaceRecognizer.exe";

    if (!fs::exists(fr_exe)) {
        std::cerr << "[FR Manager] FaceRecognizer.exe 不存在: " << fr_exe << std::endl;
        return false;
    }

    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;

    HANDLE hStdoutWrite = NULL;
    HANDLE hStderrWrite = NULL;

    if (!CreatePipe(&fr_stdout_read_, &hStdoutWrite, &saAttr, 0)) {
        std::cerr << "[FR Manager] CreatePipe(Stdout) failed" << std::endl;
        return false;
    }
    SetHandleInformation(fr_stdout_read_, HANDLE_FLAG_INHERIT, 0);

    if (!CreatePipe(&fr_stderr_read_, &hStderrWrite, &saAttr, 0)) {
        std::cerr << "[FR Manager] CreatePipe(Stderr) failed" << std::endl;
        return false;
    }
    SetHandleInformation(fr_stderr_read_, HANDLE_FLAG_INHERIT, 0);

    // 启动 FaceRecognizer 进程
    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.hStdError = hStderrWrite;
    si.hStdOutput = hStdoutWrite;
    si.dwFlags |= STARTF_USESTDHANDLES;
    
    memset(&fr_process_info_, 0, sizeof(fr_process_info_));

    // FR 默认发送到 51234，我们让它发送到 51235（SU 监听）
    // 启动识别模式(recognize)，并指定UDP端口（注意：不能传 -d 参数，因为原程序的 CLI 设计是收到设置向参数就会保存并直接 return 0）
    std::string cmd_line = "\"" + fr_exe.string() + "\" -m recognize --udp-port 51235";
    
    if (!CreateProcessA(
        nullptr,
        const_cast<char*>(cmd_line.c_str()),
        nullptr, nullptr, TRUE, 
        CREATE_NO_WINDOW,  // 无窗口模式
        nullptr, nullptr, &si, &fr_process_info_)) {
        std::cerr << "[FR Manager] 启动 FaceRecognizer.exe 失败" << std::endl;
        CloseHandle(hStdoutWrite);
        CloseHandle(hStderrWrite);
        return false;
    }

    fr_process_ = fr_process_info_.hProcess;
    
    // Close the write ends of the pipes in the parent process
    CloseHandle(hStdoutWrite);
    CloseHandle(hStderrWrite);

    std::cout << "[FR Manager] FaceRecognizer.exe 已启动 (PID: " 
              << fr_process_info_.dwProcessId << ")" << std::endl;
    
    // 开启日志读取线程
    log_thread_running_ = true;
    log_thread_ = std::thread(&FaceRecognition::ReadSubProcessLogs, this);
    
    // 等待 FR 启动
    Sleep(1500);
    
    return true;
}

void FaceRecognition::ReadSubProcessLogs() {
    char buffer[4096];
    DWORD bytesRead;
    
    while(log_thread_running_) {
        // Here we read stdout. Since ReadFile is blocking, 
        // we'll check if there's data available first to allow graceful exit.
        DWORD bytesAvail = 0;
        if (PeekNamedPipe(fr_stdout_read_, NULL, 0, NULL, &bytesAvail, NULL) && bytesAvail > 0) {
            if (ReadFile(fr_stdout_read_, buffer, sizeof(buffer)-1, &bytesRead, NULL) && bytesRead > 0) {
                buffer[bytesRead] = '\0';
                std::cout << "[FR OUT]: " << buffer;
            }
        }
        
        DWORD errAvail = 0;
        if (PeekNamedPipe(fr_stderr_read_, NULL, 0, NULL, &errAvail, NULL) && errAvail > 0) {
            if (ReadFile(fr_stderr_read_, buffer, sizeof(buffer)-1, &bytesRead, NULL) && bytesRead > 0) {
                buffer[bytesRead] = '\0';
                std::cerr << "[FR ERR]: " << buffer;
            }
        }
        
        // checking the process exit status
        DWORD exitCode = 0;
        if (GetExitCodeProcess(fr_process_, &exitCode) && exitCode != STILL_ACTIVE) {
            break;  // target process closed
        }
        
        Sleep(50); // prevent CPU spin
    }
}

void FaceRecognition::stop_fr_process() {
    if (fr_process_) {
        // 发送停止状态（优雅关闭）
        if (udp_sender_) {
            udp_sender_->send_status(RecognitionStatus::PROCESS_ENDED, "");
        }
        
        // 等待进程退出
        if (WaitForSingleObject(fr_process_, 3000) == WAIT_TIMEOUT) {
            // 超时则强制终止
            TerminateProcess(fr_process_, 0);
        }
        
        CloseHandle(fr_process_info_.hProcess);
        CloseHandle(fr_process_info_.hThread);
        
        fr_process_ = nullptr;
    }
    
    log_thread_running_ = false;
    if (log_thread_.joinable()) {
        log_thread_.join();
    }
    if (fr_stdout_read_) {
        CloseHandle(fr_stdout_read_);
        fr_stdout_read_ = nullptr;
    }
    if (fr_stderr_read_) {
        CloseHandle(fr_stderr_read_);
        fr_stderr_read_ = nullptr;
    }
}

void FaceRecognition::on_fr_status_received(RecognitionStatus status, const std::string& username) {
    std::cout << "[FR Manager] 收到状态: " << static_cast<int>(status) 
              << ", 用户: " << username << std::endl;

    // 更新内部状态
    last_result_.success = (status == RecognitionStatus::SUCCESS);
    last_result_.username = username;
    last_result_.confidence = 0.0f;  // UDP 包中没有置信度信息
    last_result_.error_message = (status == RecognitionStatus::SUCCESS) ? "" : "Recognition failed";

    // 转发给 CredentialProvider
    if (udp_sender_) {
        udp_sender_->send_status(status, username);
        std::cout << "[FR Manager] 已转发状态到 CP (端口 51234)" << std::endl;
    }
}

bool FaceRecognition::Initialize(std::string& error_message) {
    if (initialized_) {
        return true;
    }

    try {
        // 1. 创建 UDP 接收器（监听 CP 请求）
        udp_receiver_cp_ = std::make_unique<UdpReceiverFromCP>(51236);
        udp_receiver_cp_->set_callback(
            [this](AuthRequestType type, const std::string& username_hint, uint32_t session_id) {
                on_cp_request_received(type, username_hint, session_id);
            }
        );
        udp_receiver_cp_->start();
        std::cout << "[FR Manager] CP 请求接收器已启动 (端口 51236)" << std::endl;

        // 2. 创建 UDP 接收器（监听 FR 结果）
        udp_receiver_fr_ = std::make_unique<UdpReceiverFromFR>(51235);
        udp_receiver_fr_->set_callback(
            [this](RecognitionStatus status, const std::string& username) {
                on_fr_status_received(status, username);
            }
        );
        udp_receiver_fr_->start();
        std::cout << "[FR Manager] FR 结果接收器已启动 (端口 51235)" << std::endl;

        // 3. 创建 UDP 发送器（发送给 CP）
        udp_sender_ = std::make_unique<UdpSenderToCP>("127.0.0.1", 51234);
        std::cout << "[FR Manager] UDP 发送器已就绪 (目标端口 51234)" << std::endl;

        initialized_ = true;
        error_message = "人脸识别模块已初始化（中间人模式，等待 CP 请求）";
        std::cout << "[FR Manager] ✅ 初始化完成 - 等待 CP 发送认证请求" << std::endl;
        return true;
    }
    catch (const std::exception& e) {
        error_message = std::string("初始化失败: ") + e.what();
        if (udp_receiver_cp_) {
            udp_receiver_cp_->stop();
        }
        if (udp_receiver_fr_) {
            udp_receiver_fr_->stop();
        }
        return false;
    }
}

bool FaceRecognition::StartRecognition() {
    if (!initialized_) {
        std::cerr << "[FR Manager] 未初始化" << std::endl;
        return false;
    }

    if (!fr_process_) {
        std::cout << "[FR Manager] 准备启动 FaceRecognizer 进程进行测试..." << std::endl;
        if (!start_fr_process()) {
            std::cerr << "[FR Manager] 启动 FR 失败" << std::endl;
            return false;
        }
    }

    // FR 启动后会自动开始识别，这里只是标记状态
    is_running_ = true;
    std::cout << "[FR Manager] 识别已开始（等待 FR 结果）" << std::endl;
    return true;
}

void FaceRecognition::StopRecognition() {
    if (!is_running_) {
        return;
    }

    is_running_ = false;
    stop_fr_process();
    std::cout << "[FR Manager] 识别已停止" << std::endl;
}RecognitionResult FaceRecognition::GetLastResult() const {
    return last_result_;
}

std::string FaceRecognition::ExtractFaceFeature(const std::string& image_path) {
    if (!initialized_) {
        return "";
    }

    // TODO: 需要 FR 支持命令模式（通过文件或管道）
    // 当前 FR 只支持实时识别，不支持单张图片特征提取
    
    std::cerr << "[FR Manager] ExtractFaceFeature 暂不支持（需要 FR 命令模式）" << std::endl;
    return "";
}

bool FaceRecognition::CompareFaces(const std::string& feature1, const std::string& feature2, float& similarity) {
    if (!initialized_ || feature1.empty() || feature2.empty()) {
        similarity = 0.0f;
        return false;
    }

    // TODO: 需要 FR 支持命令模式
    std::cerr << "[FR Manager] CompareFaces 暂不支持（需要 FR 命令模式）" << std::endl;
    
    similarity = 0.0f;
    return false;
}

void FaceRecognition::on_cp_request_received(AuthRequestType type, const std::string& username_hint, uint32_t session_id) {
    std::cout << "[FR Manager] ========================================" << std::endl;
    std::cout << "[FR Manager] 收到 CP 请求: type=" << static_cast<int>(type) 
              << ", session=" << session_id 
              << ", hint=" << username_hint << std::endl;
    
    current_session_id_ = session_id;

    switch (type) {
        case AuthRequestType::START_RECOGNITION:
            std::cout << "[FR Manager] 处理开始识别请求..." << std::endl;
            
            // 如果 FR 进程未运行，启动它
            if (!fr_process_) {
                std::cout << "[FR Manager] 启动 FaceRecognizer 进程..." << std::endl;
                if (!start_fr_process()) {
                    std::cerr << "[FR Manager] 启动 FR 失败，发送错误状态" << std::endl;
                    if (udp_sender_) {
                        udp_sender_->send_status(RecognitionStatus::RECOGNITION_ERROR, "");
                    }
                    return;
                }
            }
            
            // FR 会自动开始识别并发送结果到 51235
            std::cout << "[FR Manager] FR 进程已就绪，等待识别结果..." << std::endl;
            is_running_ = true;
            
            // 发送 RECOGNIZING 状态给 CP
            if (udp_sender_) {
                udp_sender_->send_status(RecognitionStatus::RECOGNIZING, "");
            }
            break;

        case AuthRequestType::CANCEL_RECOGNITION:
            std::cout << "[FR Manager] 处理取消识别请求..." << std::endl;
            StopRecognition();
            if (udp_sender_) {
                udp_sender_->send_status(RecognitionStatus::IDLE, "");
            }
            break;

        case AuthRequestType::QUERY_STATUS:
            std::cout << "[FR Manager] 处理查询状态请求..." << std::endl;
            // 返回当前状态
            if (udp_sender_) {
                RecognitionStatus current_status = is_running_ ? 
                    RecognitionStatus::RECOGNIZING : RecognitionStatus::IDLE;
                udp_sender_->send_status(current_status, last_result_.username);
            }
            break;

        default:
            std::cerr << "[FR Manager] 未知请求类型: " << static_cast<int>(type) << std::endl;
            break;
    }
    
    std::cout << "[FR Manager] ========================================" << std::endl;
}

} // namespace managers
} // namespace smile2unlock
