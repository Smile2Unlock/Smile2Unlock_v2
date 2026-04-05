#pragma once

#include "models/gui_ipc_protocol.h"
#include "ibackend_service.h"
#include <windows.h>
#include <string>
#include <vector>
#include <memory>
#include <iostream>
#include <sstream>
#include <cstring>
#include <chrono>

namespace smile2unlock {

/**
 * @brief 远程后端服务 - 通过 IPC 连接到已运行的 Service
 * 
 * 在 GUI 模式下，如果检测到已有 Service 进程运行，
 * 则使用此类通过命名管道与 Service 通信
 */
class RemoteBackendService : public IBackendService {
public:
    RemoteBackendService() : pipe_(INVALID_HANDLE_VALUE), request_id_(0), initialized_(false) {}
    
    ~RemoteBackendService() override {
        disconnect();
    }
    
    bool Initialize() override {
        if (initialized_) return true;
        
        if (!connect()) {
            return false;
        }
        
        initialized_ = true;
        return true;
    }
    
    bool IsInitialized() const override {
        return initialized_;
    }
    
    // ==================== DLL 管理 ====================
    
    DllStatus GetDllStatus() override {
        GuiIpcResponse response;
        std::string payload;
        
        if (!send_request(GuiIpcCommand::GET_DLL_STATUS, "", response)) {
            return DllStatus{};
        }
        
        DllStatus status;
        parse_dll_status(response.payload, status);
        return status;
    }
    
    bool CalculateDllHashes() override {
        return true;
    }
    
    bool InjectDll(std::string& error_message) override {
        GuiIpcResponse response;
        if (!send_request(GuiIpcCommand::INJECT_DLL, "", response)) {
            error_message = response.payload;
            return false;
        }
        error_message = response.payload;
        return response.status == static_cast<int32_t>(GuiIpcStatus::SUCCESS);
    }
    
    // ==================== 用户管理 ====================
    
    std::vector<User> GetAllUsers() override {
        GuiIpcResponse response;
        if (!send_request(GuiIpcCommand::GET_ALL_USERS, "", response)) {
            return {};
        }
        return parse_users(response.payload);
    }
    
    bool AddUser(const std::string& username, const std::string& password, 
                 const std::string& remark, std::string& error_message) override {
        std::string payload = username + "|" + password + "|" + remark;
        GuiIpcResponse response;
        
        if (!send_request(GuiIpcCommand::ADD_USER, payload, response)) {
            error_message = response.payload;
            return false;
        }
        error_message = response.payload;
        return response.status == static_cast<int32_t>(GuiIpcStatus::SUCCESS);
    }
    
    bool UpdateUser(int user_id, const std::string& username, const std::string& password,
                    const std::string& remark, std::string& error_message) override {
        std::string payload = std::to_string(user_id) + "|" + username + "|" + password + "|" + remark;
        GuiIpcResponse response;
        
        if (!send_request(GuiIpcCommand::UPDATE_USER, payload, response)) {
            error_message = response.payload;
            return false;
        }
        error_message = response.payload;
        return response.status == static_cast<int32_t>(GuiIpcStatus::SUCCESS);
    }
    
    bool DeleteUser(int userId, std::string& error_message) override {
        std::string payload = std::to_string(userId);
        GuiIpcResponse response;
        
        if (!send_request(GuiIpcCommand::DELETE_USER, payload, response)) {
            error_message = response.payload;
            return false;
        }
        error_message = response.payload;
        return response.status == static_cast<int32_t>(GuiIpcStatus::SUCCESS);
    }
    
    // ==================== 人脸管理 ====================
    
    bool AddFace(int user_id, const std::string& feature, 
                 const std::string& remark, std::string& error_message) override {
        error_message = "远程模式不支持直接添加人脸特征";
        return false;
    }
    
    bool StartCameraPreview(std::string& error_message) override {
        GuiIpcResponse response;
        if (!send_request(GuiIpcCommand::START_CAMERA_PREVIEW, "", response)) {
            error_message = response.payload;
            return false;
        }
        error_message = response.payload;
        return response.status == static_cast<int32_t>(GuiIpcStatus::SUCCESS);
    }
    
    void StopCameraPreview() override {
        GuiIpcResponse response;
        send_request(GuiIpcCommand::STOP_CAMERA_PREVIEW, "", response);
    }
    
    bool IsCameraPreviewRunning() const override {
        GuiIpcResponse response;
        if (!const_cast<RemoteBackendService*>(this)->send_request(
                GuiIpcCommand::IS_CAMERA_PREVIEW_RUNNING, "", response)) {
            return false;
        }
        return std::string(response.payload) == "1";
    }
    
    bool GetLatestCameraPreview(std::vector<unsigned char>& image_data, 
                                int& width, int& height, 
                                std::string& error_message) override {
        GuiIpcResponse response;
        if (!send_request(GuiIpcCommand::GET_LATEST_PREVIEW, "", response)) {
            error_message = response.payload;
            return false;
        }
        // 解析预览数据：宽x高x数据(十六进制)
        return parse_preview_data(std::string(response.payload, response.payload_size), image_data, width, height, error_message);
    }
    
    bool CapturePreviewFrame(std::vector<unsigned char>& image_data,
                             int& width, int& height,
                             std::string& error_message) override {
        GuiIpcResponse response;
        if (!send_request(GuiIpcCommand::CAPTURE_PREVIEW_FRAME, "", response)) {
            error_message = response.payload;
            return false;
        }
        return parse_preview_data(response.payload, image_data, width, height, error_message);
    }
    
    bool CaptureAndAddFace(int user_id, const std::string& remark,
                          std::string& error_message) override {
        std::string payload = std::to_string(user_id) + "|" + remark;
        GuiIpcResponse response;
        
        if (!send_request(GuiIpcCommand::CAPTURE_AND_ADD_FACE, payload, response)) {
            error_message = response.payload;
            return false;
        }
        error_message = response.payload;
        return response.status == static_cast<int32_t>(GuiIpcStatus::SUCCESS);
    }
    
    bool DeleteFace(int user_id, int face_id, std::string& error_message) override {
        std::string payload = std::to_string(user_id) + "|" + std::to_string(face_id);
        GuiIpcResponse response;
        
        if (!send_request(GuiIpcCommand::DELETE_FACE, payload, response)) {
            error_message = response.payload;
            return false;
        }
        error_message = response.payload;
        return response.status == static_cast<int32_t>(GuiIpcStatus::SUCCESS);
    }
    
    bool UpdateFaceRemark(int user_id, int face_id, const std::string& remark,
                         std::string& error_message) override {
        error_message = "远程模式暂不支持更新人脸备注";
        return false;
    }
    
    std::vector<FaceData> GetUserFaces(int user_id) override {
        std::string payload = std::to_string(user_id);
        GuiIpcResponse response;
        
        if (!send_request(GuiIpcCommand::GET_USER_FACES, payload, response)) {
            return {};
        }
        return parse_faces(response.payload);
    }
    
    // ==================== 人脸识别 ====================
    
    bool StartRecognition(std::string& error_message) override {
        GuiIpcResponse response;
        if (!send_request(GuiIpcCommand::START_RECOGNITION, "", response)) {
            error_message = response.payload;
            return false;
        }
        error_message = response.payload;
        return response.status == static_cast<int32_t>(GuiIpcStatus::SUCCESS);
    }
    
    void StopRecognition() override {
        GuiIpcResponse response;
        send_request(GuiIpcCommand::STOP_RECOGNITION, "", response);
    }
    
    bool IsRecognitionRunning() const override {
        GuiIpcResponse response;
        if (!const_cast<RemoteBackendService*>(this)->send_request(
                GuiIpcCommand::IS_RECOGNITION_RUNNING, "", response)) {
            return false;
        }
        return std::string(response.payload) == "1";
    }
    
    RecognitionResult GetRecognitionResult() override {
        GuiIpcResponse response;
        if (!send_request(GuiIpcCommand::GET_RECOGNITION_RESULT, "", response)) {
            return RecognitionResult{};
        }
        return parse_recognition_result(response.payload);
    }

private:
    HANDLE pipe_;
    int32_t request_id_;
    bool initialized_;
    
    bool connect() {
        if (pipe_ != INVALID_HANDLE_VALUE) {
            return true;
        }
        
        // 等待管道可用
        if (!WaitNamedPipeA(GUI_IPC_PIPE_NAME, 3000)) {
            std::cerr << "[RemoteBackend] 等待管道超时" << std::endl;
            return false;
        }
        
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
            std::cerr << "[RemoteBackend] 连接管道失败: " << GetLastError() << std::endl;
            return false;
        }
        
        DWORD mode = PIPE_READMODE_MESSAGE;
        SetNamedPipeHandleState(pipe_, &mode, nullptr, nullptr);
        
        std::cout << "[RemoteBackend] 已连接到 Service" << std::endl;
        return true;
    }
    
    void disconnect() {
        if (pipe_ != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe_);
            pipe_ = INVALID_HANDLE_VALUE;
            initialized_ = false;
        }
    }
    
    bool send_request(GuiIpcCommand cmd, const std::string& payload, GuiIpcResponse& response) {
        if (!initialized_ && !connect()) {
            response.set_error("Not connected");
            return false;
        }
        
        GuiIpcRequest request;
        request.clear();
        request.command = static_cast<int32_t>(cmd);
        request.request_id = ++request_id_;
        request.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        
        if (!payload.empty()) {
            size_t copy_size = std::min(payload.size(), sizeof(request.payload) - 1);
            std::memcpy(request.payload, payload.data(), copy_size);
            request.payload_size = static_cast<int32_t>(copy_size);
        }
        
        DWORD bytes_written = 0;
        if (!WriteFile(pipe_, &request, sizeof(request), &bytes_written, nullptr)) {
            disconnect();
            response.set_error("Write failed");
            return false;
        }
        
        DWORD bytes_read = 0;
        if (!ReadFile(pipe_, &response, sizeof(response), &bytes_read, nullptr)) {
            disconnect();
            response.set_error("Read failed");
            return false;
        }
        
        return response.status == static_cast<int32_t>(GuiIpcStatus::SUCCESS);
    }
    
    // 解析用户列表
    std::vector<User> parse_users(const std::string& data) {
        std::vector<User> users;
        std::istringstream ss(data);
        std::string line;
        
        while (std::getline(ss, line)) {
            if (line.empty()) continue;
            
            User user;
            size_t pos = 0;
            size_t prev = 0;
            int field = 0;
            
            while ((pos = line.find('\t', prev)) != std::string::npos) {
                std::string field_val = line.substr(prev, pos - prev);
                switch (field) {
                    case 0: user.id = std::stoi(field_val); break;
                    case 1: user.username = field_val; break;
                    case 2: /* encrypted_password 跳过 */ break;
                    case 3: user.remark = field_val; break;
                }
                prev = pos + 1;
                field++;
            }
            
            if (!user.username.empty()) {
                users.push_back(user);
            }
        }
        
        return users;
    }
    
    // 解析人脸列表
    std::vector<FaceData> parse_faces(const std::string& data) {
        std::vector<FaceData> faces;
        std::istringstream ss(data);
        std::string line;
        
        while (std::getline(ss, line)) {
            if (line.empty()) continue;
            
            FaceData face;
            size_t pos = 0;
            size_t prev = 0;
            int field = 0;
            
            while ((pos = line.find('\t', prev)) != std::string::npos) {
                std::string field_val = line.substr(prev, pos - prev);
                switch (field) {
                    case 0: face.id = std::stoi(field_val); break;
                    case 1: face.remark = field_val; break;
                }
                prev = pos + 1;
                field++;
            }
            
            faces.push_back(face);
        }
        
        return faces;
    }
    
    // 解析 DLL 状态
    void parse_dll_status(const std::string& data, DllStatus& status) {
        std::istringstream ss(data);
        std::string line;
        
        while (std::getline(ss, line)) {
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            
            if (key == "is_injected") status.is_injected = (val == "1");
            else if (key == "is_registry_configured") status.is_registry_configured = (val == "1");
            else if (key == "source_path") status.source_path = val;
            else if (key == "target_path") status.target_path = val;
            else if (key == "source_hash") status.source_hash = val;
            else if (key == "target_hash") status.target_hash = val;
            else if (key == "source_version") status.source_version = val;
            else if (key == "target_version") status.target_version = val;
        }
    }
    
    // 解析预览数据 - 二进制格式
    bool parse_preview_data(const std::string& data, std::vector<unsigned char>& image_data,
                           int& width, int& height, std::string& error_message) {
        // 二进制格式: [4 bytes: width][4 bytes: height][4 bytes: data_size][data_size bytes: image_data]
        const size_t kHeaderSize = 12;
        
        if (data.size() < kHeaderSize) {
            error_message = "Invalid preview data: too short";
            return false;
        }
        
        // 读取二进制 header
        const int32_t* header = reinterpret_cast<const int32_t*>(data.data());
        width = header[0];
        height = header[1];
        int32_t data_size = header[2];
        
        // 验证数据大小
        if (data_size < 0 || static_cast<size_t>(kHeaderSize + data_size) > data.size()) {
            error_message = "Invalid preview data: size mismatch";
            return false;
        }
        
        // 读取图像数据
        image_data.clear();
        if (data_size > 0) {
            image_data.assign(
                reinterpret_cast<const unsigned char*>(data.data()) + kHeaderSize,
                reinterpret_cast<const unsigned char*>(data.data()) + kHeaderSize + data_size
            );
        }
        
        return true;
    }
    
    // 解析识别结果
    RecognitionResult parse_recognition_result(const std::string& data) {
        RecognitionResult result;
        std::istringstream ss(data);
        std::string line;
        
        while (std::getline(ss, line)) {
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            
            if (key == "success") result.success = (val == "1");
            else if (key == "username") result.username = val;
            else if (key == "confidence") result.confidence = std::stof(val);
            else if (key == "error_message") result.error_message = val;
        }
        
        return result;
    }
};

} // namespace smile2unlock
