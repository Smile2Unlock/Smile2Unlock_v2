#pragma once

#include "models/gui_ipc_protocol.h"
#include <windows.h>
#include <string>
#include <iostream>
#include <chrono>

namespace smile2unlock {

/**
 * @brief GUI IPC 客户端 - 在 GUI 进程中运行
 * 
 * 通过命名管道连接到 Service 进程，发送请求并接收响应
 */
class GuiIpcClient {
public:
    GuiIpcClient() : pipe_(INVALID_HANDLE_VALUE), request_id_counter_(0) {}
    
    ~GuiIpcClient() {
        disconnect();
    }
    
    // 连接到 Service
    bool connect(int timeout_ms = 5000) {
        if (pipe_ != INVALID_HANDLE_VALUE) {
            return true;  // 已连接
        }
        
        // 等待管道可用
        if (!WaitNamedPipeA(GUI_IPC_PIPE_NAME, timeout_ms)) {
            std::cerr << "[IPC Client] 等待管道超时" << std::endl;
            return false;
        }
        
        // 连接管道
        pipe_ = CreateFileA(
            GUI_IPC_PIPE_NAME,
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr
        );
        
        if (pipe_ == INVALID_HANDLE_VALUE) {
            std::cerr << "[IPC Client] 连接管道失败: " << GetLastError() << std::endl;
            return false;
        }
        
        // 设置消息模式
        DWORD mode = PIPE_READMODE_MESSAGE;
        if (!SetNamedPipeHandleState(pipe_, &mode, nullptr, nullptr)) {
            CloseHandle(pipe_);
            pipe_ = INVALID_HANDLE_VALUE;
            return false;
        }
        
        std::cout << "[IPC Client] 已连接到 Service" << std::endl;
        return true;
    }
    
    void disconnect() {
        if (pipe_ != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe_);
            pipe_ = INVALID_HANDLE_VALUE;
            std::cout << "[IPC Client] 已断开连接" << std::endl;
        }
    }
    
    bool is_connected() const {
        return pipe_ != INVALID_HANDLE_VALUE;
    }
    
    // 发送请求并接收响应
    bool send_request(GuiIpcCommand cmd, const std::string& payload, 
                      GuiIpcResponse& response, int timeout_ms = 5000) {
        if (!is_connected()) {
            response.set_error("Not connected");
            return false;
        }
        
        // 构造请求
        GuiIpcRequest request;
        request.clear();
        request.command = static_cast<int32_t>(cmd);
        request.request_id = ++request_id_counter_;
        request.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        
        if (!payload.empty()) {
            size_t copy_size = std::min(payload.size(), sizeof(request.payload) - 1);
            std::memcpy(request.payload, payload.data(), copy_size);
            request.payload_size = static_cast<int32_t>(copy_size);
        }
        
        // 发送请求
        DWORD bytes_written = 0;
        if (!WriteFile(pipe_, &request, sizeof(request), &bytes_written, nullptr)) {
            std::cerr << "[IPC Client] 发送请求失败: " << GetLastError() << std::endl;
            disconnect();
            response.set_error("Write failed");
            return false;
        }
        
        // 接收响应
        DWORD bytes_read = 0;
        if (!ReadFile(pipe_, &response, sizeof(response), &bytes_read, nullptr) || bytes_read != sizeof(response)) {
            std::cerr << "[IPC Client] 接收响应失败: " << GetLastError() << std::endl;
            disconnect();
            response.set_error("Read failed");
            return false;
        }
        
        // 验证响应
        if (response.magic != GUI_IPC_MAGIC) {
            response.set_error("Invalid response magic");
            return false;
        }
        
        if (response.request_id != request.request_id) {
            response.set_error("Request ID mismatch");
            return false;
        }
        
        return response.status == static_cast<int32_t>(GuiIpcStatus::SUCCESS);
    }
    
    // Ping 测试连接
    bool ping() {
        GuiIpcResponse response;
        return send_request(GuiIpcCommand::PING, "", response, 1000);
    }

private:
    HANDLE pipe_;
    int32_t request_id_counter_;
};

} // namespace smile2unlock
