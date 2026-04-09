#pragma once

#include "models/gui_ipc_protocol.h"
#include "utils/windows_security.h"
#include <windows.h>
#include <thread>
#include <atomic>
#include <functional>
#include <iostream>
#include <string>
#include <vector>
#include <memory>

namespace smile2unlock {

/**
 * @brief GUI IPC 服务端 - 在 Service 进程中运行
 * 
 * 监听命名管道，接收 GUI 的请求并转发给 BackendService
 */
class GuiIpcServer {
public:
    using RequestHandler = std::function<void(const GuiIpcRequest&, GuiIpcResponse&)>;
    
    GuiIpcServer() 
        : running_(false)
        , pipe_(INVALID_HANDLE_VALUE)
        , thread_() {}
    
    ~GuiIpcServer() {
        stop();
    }
    
    void set_handler(RequestHandler handler) {
        handler_ = std::move(handler);
    }
    
    bool start() {
        if (running_) return true;
        
        running_ = true;
        thread_ = std::thread([this]() { server_loop(); });
        
        std::cout << "[IPC Server] 已启动，监听: " << GUI_IPC_PIPE_NAME << std::endl;
        return true;
    }
    
    void stop() {
        if (!running_) return;
        
        running_ = false;
        
        // 关闭管道以解除阻塞的读取
        if (pipe_ != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe_);
            pipe_ = INVALID_HANDLE_VALUE;
        }
        
        if (thread_.joinable()) {
            thread_.join();
        }
        
        std::cout << "[IPC Server] 已停止" << std::endl;
    }
    
    bool is_running() const { return running_; }

private:
    static bool build_pipe_security(SECURITY_ATTRIBUTES& sa, SECURITY_DESCRIPTOR& sd, PACL& acl) {
        return windows_security::BuildServiceIpcSecurityAttributes(sa, sd, acl);
    }

    void server_loop() {
        while (running_) {
            SECURITY_ATTRIBUTES sa{};
            SECURITY_DESCRIPTOR sd{};
            PACL acl = nullptr;
            SECURITY_ATTRIBUTES* pipe_sa = nullptr;
            if (build_pipe_security(sa, sd, acl)) {
                pipe_sa = &sa;
            } else if (running_) {
                std::cerr << "[IPC Server] 构建管道安全描述符失败，使用默认安全属性: "
                          << GetLastError() << std::endl;
            }

            // 创建命名管道
            pipe_ = CreateNamedPipeA(
                GUI_IPC_PIPE_NAME,
                PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES,
                sizeof(GuiIpcResponse),
                sizeof(GuiIpcRequest),
                0,
                pipe_sa
            );
            windows_security::FreeSecurityAcl(acl);
            
            if (pipe_ == INVALID_HANDLE_VALUE) {
                if (running_) {
                    std::cerr << "[IPC Server] 创建管道失败: " << GetLastError() << std::endl;
                    Sleep(1000);
                }
                continue;
            }
            
            // 等待客户端连接
            if (!ConnectNamedPipe(pipe_, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
                CloseHandle(pipe_);
                pipe_ = INVALID_HANDLE_VALUE;
                continue;
            }
            
            // 处理请求
            handle_client();
            
            // 断开连接，准备下一次
            DisconnectNamedPipe(pipe_);
            CloseHandle(pipe_);
            pipe_ = INVALID_HANDLE_VALUE;
        }
    }
    
    void handle_client() {
        // 在同一个连接中处理多个请求
        while (running_) {
            auto request = std::make_unique<GuiIpcRequest>();
            auto response = std::make_unique<GuiIpcResponse>();
            
            DWORD bytes_read = 0;
            const BOOL read_ok = ReadFile(
                pipe_,
                request.get(),
                sizeof(*request),
                &bytes_read,
                nullptr
            );
            
            // 检查是否连接断开或出错
            if (!read_ok || bytes_read != sizeof(*request)) {
                DWORD err = GetLastError();
                if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA) {
                    // 客户端断开连接，正常退出
                    break;
                }
                if (running_) {
                    std::cerr << "[IPC Server] 读取请求失败: " << err << std::endl;
                }
                break;
            }
            
            // 验证协议
            if (request->magic != GUI_IPC_MAGIC) {
                response->set_error("Invalid magic number");
                send_response(*response);
                continue;
            }
            
            if (request->version != GUI_IPC_VERSION) {
                response->set_error("Version mismatch");
                send_response(*response);
                continue;
            }
            
            // 清空响应
            response->clear();
            response->request_id = request->request_id;
            response->timestamp = request->timestamp;
            
            // 调用处理器
            if (handler_) {
                handler_(*request, *response);
            } else {
                response->set_error("No handler registered");
            }
            
            // 发送响应
            send_response(*response);
        }
    }
    
    void send_response(const GuiIpcResponse& response) {
        DWORD bytes_written = 0;
        WriteFile(
            pipe_,
            &response,
            sizeof(response),
            &bytes_written,
            nullptr
        );
    }
    
    std::atomic<bool> running_;
    HANDLE pipe_;
    std::thread thread_;
    RequestHandler handler_;
};

} // namespace smile2unlock
