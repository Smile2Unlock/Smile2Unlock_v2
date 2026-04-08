#pragma once

#include "models/gui_ipc_protocol.h"
#include "ibackend_service.h"
#include <string>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <vector>
#include <windows.h>

namespace smile2unlock {

void NotifyServiceActivity();

/**
 * @brief IPC 请求处理器 - 在 Service 端处理 GUI 的请求
 */
class GuiIpcRequestHandler {
public:
    explicit GuiIpcRequestHandler(IBackendService* backend) : backend_(backend) {}
    
    void handle(const GuiIpcRequest& request, GuiIpcResponse& response) {
        response.clear();
        response.request_id = request.request_id;
        response.timestamp = request.timestamp;
        NotifyServiceActivity();
        
        if (!backend_) {
            response.set_error("Backend not available");
            return;
        }
        
        auto cmd = static_cast<GuiIpcCommand>(request.command);
        std::string payload(request.payload, request.payload_size);
        
        try {
            handle_command(cmd, payload, response);
        } catch (const std::exception& e) {
            response.set_error(e.what());
        }
    }

private:
    IBackendService* backend_;
    HANDLE preview_transport_mapping_{nullptr};
    HANDLE capture_transport_mapping_{nullptr};

    static bool build_shared_mapping_security(SECURITY_ATTRIBUTES& sa, SECURITY_DESCRIPTOR& sd) {
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

    static std::string last_error_message(const char* prefix) {
        std::ostringstream ss;
        ss << prefix << " (GetLastError=" << GetLastError() << ")";
        return ss.str();
    }

    void close_transport_mapping(HANDLE& mapping) {
        if (mapping != nullptr) {
            CloseHandle(mapping);
            mapping = nullptr;
        }
    }

    bool serialize_preview_via_shared_mapping(
        const std::vector<unsigned char>& image_data,
        int width,
        int height,
        GuiIpcResponse& response,
        HANDLE& mapping_slot,
        const char* mapping_tag
    ) {
        close_transport_mapping(mapping_slot);

        SECURITY_ATTRIBUTES sa{};
        SECURITY_DESCRIPTOR sd{};
        if (!build_shared_mapping_security(sa, sd)) {
            response.set_error(last_error_message("构建共享内存安全描述符失败").c_str());
            return false;
        }

        const std::string mapping_name =
            std::string("Local\\Smile2Unlock_GuiPreview_") +
            mapping_tag + "_" + std::to_string(GetCurrentProcessId());
        const DWORD mapping_size = static_cast<DWORD>(image_data.size());
        mapping_slot = CreateFileMappingA(
            INVALID_HANDLE_VALUE,
            &sa,
            PAGE_READWRITE,
            0,
            mapping_size == 0 ? 1 : mapping_size,
            mapping_name.c_str()
        );
        if (!mapping_slot) {
            response.set_error(last_error_message("创建 GUI 预览共享内存失败").c_str());
            return false;
        }

        void* view = MapViewOfFile(mapping_slot, FILE_MAP_ALL_ACCESS, 0, 0, mapping_size == 0 ? 1 : mapping_size);
        if (!view) {
            close_transport_mapping(mapping_slot);
            response.set_error(last_error_message("映射 GUI 预览共享内存失败").c_str());
            return false;
        }

        if (!image_data.empty()) {
            std::memcpy(view, image_data.data(), image_data.size());
        }
        UnmapViewOfFile(view);

        std::ostringstream ss;
        ss << "transport=shared_memory\n";
        ss << "map_name=" << mapping_name << "\n";
        ss << "width=" << width << "\n";
        ss << "height=" << height << "\n";
        ss << "channels=3\n";
        ss << "image_bytes=" << image_data.size() << "\n";
        ss << "compression=0\n";
        const std::string descriptor = ss.str();
        std::strncpy(response.payload, descriptor.c_str(), sizeof(response.payload) - 1);
        response.payload_size = static_cast<int32_t>(descriptor.size());
        response.status = static_cast<int32_t>(GuiIpcStatus::SUCCESS);
        return true;
    }
    
    void handle_command(GuiIpcCommand cmd, const std::string& payload, GuiIpcResponse& response) {
        switch (cmd) {
            case GuiIpcCommand::PING:
                response.status = static_cast<int32_t>(GuiIpcStatus::SUCCESS);
                std::strcpy(response.payload, "pong");
                response.payload_size = 4;
                break;
                
            case GuiIpcCommand::GET_ALL_USERS:
                handle_get_all_users(response);
                break;
                
            case GuiIpcCommand::ADD_USER:
                handle_add_user(payload, response);
                break;
                
            case GuiIpcCommand::UPDATE_USER:
                handle_update_user(payload, response);
                break;
                
            case GuiIpcCommand::DELETE_USER:
                handle_delete_user(payload, response);
                break;
                
            case GuiIpcCommand::GET_USER_FACES:
                handle_get_user_faces(payload, response);
                break;
                
            case GuiIpcCommand::DELETE_FACE:
                handle_delete_face(payload, response);
                break;
                
            case GuiIpcCommand::CAPTURE_AND_ADD_FACE:
                handle_capture_and_add_face(payload, response);
                break;
                
            case GuiIpcCommand::START_CAMERA_PREVIEW:
                handle_start_camera_preview(response);
                break;
                
            case GuiIpcCommand::STOP_CAMERA_PREVIEW:
                backend_->StopCameraPreview();
                response.status = static_cast<int32_t>(GuiIpcStatus::SUCCESS);
                break;
                
            case GuiIpcCommand::IS_CAMERA_PREVIEW_RUNNING:
                handle_is_camera_preview_running(response);
                break;
                
            case GuiIpcCommand::GET_LATEST_PREVIEW:
                handle_get_latest_preview(response);
                break;
                
            case GuiIpcCommand::CAPTURE_PREVIEW_FRAME:
                handle_capture_preview_frame(response);
                break;
                
            case GuiIpcCommand::GET_DLL_STATUS:
                handle_get_dll_status(response);
                break;
                
            case GuiIpcCommand::INJECT_DLL:
                handle_inject_dll(response);
                break;
                
            case GuiIpcCommand::START_RECOGNITION:
                handle_start_recognition(response);
                break;
                
            case GuiIpcCommand::STOP_RECOGNITION:
                backend_->StopRecognition();
                response.status = static_cast<int32_t>(GuiIpcStatus::SUCCESS);
                break;
                
            case GuiIpcCommand::IS_RECOGNITION_RUNNING:
                handle_is_recognition_running(response);
                break;
                
            case GuiIpcCommand::GET_RECOGNITION_RESULT:
                handle_get_recognition_result(response);
                break;

            case GuiIpcCommand::GET_RECOGNIZER_CONFIG:
                handle_get_recognizer_config(response);
                break;

            case GuiIpcCommand::SAVE_RECOGNIZER_CONFIG:
                handle_save_recognizer_config(payload, response);
                break;
                
            default:
                response.set_error("Unknown command");
                break;
        }
    }
    
    // ==================== 用户管理 ====================
    
    void handle_get_all_users(GuiIpcResponse& response) {
        auto users = backend_->GetAllUsers();
        std::ostringstream ss;
        
        for (const auto& user : users) {
            ss << user.id << "\t" << user.username << "\t" 
               << user.encrypted_password << "\t" << user.remark << "\n";
        }
        
        std::string result = ss.str();
        std::strncpy(response.payload, result.c_str(), sizeof(response.payload) - 1);
        response.payload_size = static_cast<int32_t>(result.size());
        response.status = static_cast<int32_t>(GuiIpcStatus::SUCCESS);
    }
    
    void handle_add_user(const std::string& payload, GuiIpcResponse& response) {
        size_t p1 = payload.find('|');
        size_t p2 = payload.find('|', p1 + 1);
        
        if (p1 == std::string::npos || p2 == std::string::npos) {
            response.set_error("Invalid payload format");
            return;
        }
        
        std::string username = payload.substr(0, p1);
        std::string password = payload.substr(p1 + 1, p2 - p1 - 1);
        std::string remark = payload.substr(p2 + 1);
        
        std::string error;
        bool success = backend_->AddUser(username, password, remark, error);
        
        std::strncpy(response.payload, error.c_str(), sizeof(response.payload) - 1);
        response.payload_size = static_cast<int32_t>(error.size());
        response.status = success ? static_cast<int32_t>(GuiIpcStatus::SUCCESS) 
                                  : static_cast<int32_t>(GuiIpcStatus::SERVICE_ERROR);
    }
    
    void handle_update_user(const std::string& payload, GuiIpcResponse& response) {
        size_t p1 = payload.find('|');
        size_t p2 = payload.find('|', p1 + 1);
        size_t p3 = payload.find('|', p2 + 1);
        
        if (p1 == std::string::npos || p2 == std::string::npos || p3 == std::string::npos) {
            response.set_error("Invalid payload format");
            return;
        }
        
        int user_id = std::stoi(payload.substr(0, p1));
        std::string username = payload.substr(p1 + 1, p2 - p1 - 1);
        std::string password = payload.substr(p2 + 1, p3 - p2 - 1);
        std::string remark = payload.substr(p3 + 1);
        
        std::string error;
        bool success = backend_->UpdateUser(user_id, username, password, remark, error);
        
        std::strncpy(response.payload, error.c_str(), sizeof(response.payload) - 1);
        response.payload_size = static_cast<int32_t>(error.size());
        response.status = success ? static_cast<int32_t>(GuiIpcStatus::SUCCESS) 
                                  : static_cast<int32_t>(GuiIpcStatus::SERVICE_ERROR);
    }
    
    void handle_delete_user(const std::string& payload, GuiIpcResponse& response) {
        int user_id = std::stoi(payload);
        
        std::string error;
        bool success = backend_->DeleteUser(user_id, error);
        
        std::strncpy(response.payload, error.c_str(), sizeof(response.payload) - 1);
        response.payload_size = static_cast<int32_t>(error.size());
        response.status = success ? static_cast<int32_t>(GuiIpcStatus::SUCCESS) 
                                  : static_cast<int32_t>(GuiIpcStatus::SERVICE_ERROR);
    }
    
    // ==================== 人脸管理 ====================
    
    void handle_get_user_faces(const std::string& payload, GuiIpcResponse& response) {
        int user_id = std::stoi(payload);
        auto faces = backend_->GetUserFaces(user_id);
        
        std::ostringstream ss;
        for (const auto& face : faces) {
            ss << face.id << "\t" << face.remark << "\n";
        }
        
        std::string result = ss.str();
        std::strncpy(response.payload, result.c_str(), sizeof(response.payload) - 1);
        response.payload_size = static_cast<int32_t>(result.size());
        response.status = static_cast<int32_t>(GuiIpcStatus::SUCCESS);
    }
    
    void handle_delete_face(const std::string& payload, GuiIpcResponse& response) {
        size_t p = payload.find('|');
        if (p == std::string::npos) {
            response.set_error("Invalid payload format");
            return;
        }
        
        int user_id = std::stoi(payload.substr(0, p));
        int face_id = std::stoi(payload.substr(p + 1));
        
        std::string error;
        bool success = backend_->DeleteFace(user_id, face_id, error);
        
        std::strncpy(response.payload, error.c_str(), sizeof(response.payload) - 1);
        response.payload_size = static_cast<int32_t>(error.size());
        response.status = success ? static_cast<int32_t>(GuiIpcStatus::SUCCESS) 
                                  : static_cast<int32_t>(GuiIpcStatus::SERVICE_ERROR);
    }
    
    void handle_capture_and_add_face(const std::string& payload, GuiIpcResponse& response) {
        size_t p = payload.find('|');
        if (p == std::string::npos) {
            response.set_error("Invalid payload format");
            return;
        }
        
        int user_id = std::stoi(payload.substr(0, p));
        std::string remark = payload.substr(p + 1);
        
        std::string error;
        bool success = backend_->CaptureAndAddFace(user_id, remark, error);
        
        std::strncpy(response.payload, error.c_str(), sizeof(response.payload) - 1);
        response.payload_size = static_cast<int32_t>(error.size());
        response.status = success ? static_cast<int32_t>(GuiIpcStatus::SUCCESS) 
                                  : static_cast<int32_t>(GuiIpcStatus::SERVICE_ERROR);
    }
    
    // ==================== 摄像头/预览 ====================
    
    void handle_start_camera_preview(GuiIpcResponse& response) {
        std::string error;
        bool success = backend_->StartCameraPreview(error);
        
        std::strncpy(response.payload, error.c_str(), sizeof(response.payload) - 1);
        response.payload_size = static_cast<int32_t>(error.size());
        response.status = success ? static_cast<int32_t>(GuiIpcStatus::SUCCESS) 
                                  : static_cast<int32_t>(GuiIpcStatus::SERVICE_ERROR);
    }
    
    void handle_is_camera_preview_running(GuiIpcResponse& response) {
        bool running = backend_->IsCameraPreviewRunning();
        std::strcpy(response.payload, running ? "1" : "0");
        response.payload_size = 1;
        response.status = static_cast<int32_t>(GuiIpcStatus::SUCCESS);
    }
    
    void handle_get_latest_preview(GuiIpcResponse& response) {
        std::vector<unsigned char> image_data;
        int width = 0, height = 0;
        std::string error;
        
        if (backend_->GetLatestCameraPreview(image_data, width, height, error)) {
            serialize_preview_via_shared_mapping(image_data, width, height, response, preview_transport_mapping_, "stream");
        } else {
            std::strncpy(response.payload, error.c_str(), sizeof(response.payload) - 1);
            response.payload_size = static_cast<int32_t>(error.size());
            response.status = static_cast<int32_t>(GuiIpcStatus::SERVICE_ERROR);
        }
    }
    
    void handle_capture_preview_frame(GuiIpcResponse& response) {
        std::vector<unsigned char> image_data;
        int width = 0, height = 0;
        std::string error;
        
        if (backend_->CapturePreviewFrame(image_data, width, height, error)) {
            serialize_preview_via_shared_mapping(image_data, width, height, response, capture_transport_mapping_, "capture");
        } else {
            std::strncpy(response.payload, error.c_str(), sizeof(response.payload) - 1);
            response.payload_size = static_cast<int32_t>(error.size());
            response.status = static_cast<int32_t>(GuiIpcStatus::SERVICE_ERROR);
        }
    }
    
    // 二进制预览数据格式: [4 bytes: width][4 bytes: height][4 bytes: data_size][data_size bytes: image_data]
    void serialize_preview_data(const std::vector<unsigned char>& image_data,
                               int width, int height, GuiIpcResponse& response) {
        // 计算总大小: 12 字节 header + 图像数据
        const size_t kHeaderSize = 12;
        const size_t total_size = kHeaderSize + image_data.size();
        
        if (total_size > sizeof(response.payload)) {
            response.set_error("Preview data too large");
            return;
        }
        
        // 写入二进制 header
        int32_t* header = reinterpret_cast<int32_t*>(response.payload);
        header[0] = width;
        header[1] = height;
        header[2] = static_cast<int32_t>(image_data.size());
        
        // 写入图像数据
        if (!image_data.empty()) {
            std::memcpy(response.payload + kHeaderSize, image_data.data(), image_data.size());
        }
        
        response.payload_size = static_cast<int32_t>(total_size);
        response.status = static_cast<int32_t>(GuiIpcStatus::SUCCESS);
    }
    
    // ==================== DLL 管理 ====================
    
    void handle_get_dll_status(GuiIpcResponse& response) {
        auto status = backend_->GetDllStatus();
        
        std::ostringstream ss;
        ss << "is_injected=" << (status.is_injected ? "1" : "0") << "\n";
        ss << "is_registry_configured=" << (status.is_registry_configured ? "1" : "0") << "\n";
        ss << "source_path=" << status.source_path << "\n";
        ss << "target_path=" << status.target_path << "\n";
        ss << "source_hash=" << status.source_hash << "\n";
        ss << "target_hash=" << status.target_hash << "\n";
        ss << "source_version=" << status.source_version << "\n";
        ss << "target_version=" << status.target_version << "\n";
        
        std::string result = ss.str();
        std::strncpy(response.payload, result.c_str(), sizeof(response.payload) - 1);
        response.payload_size = static_cast<int32_t>(result.size());
        response.status = static_cast<int32_t>(GuiIpcStatus::SUCCESS);
    }
    
    void handle_inject_dll(GuiIpcResponse& response) {
        std::string error;
        bool success = backend_->InjectDll(error);
        
        std::strncpy(response.payload, error.c_str(), sizeof(response.payload) - 1);
        response.payload_size = static_cast<int32_t>(error.size());
        response.status = success ? static_cast<int32_t>(GuiIpcStatus::SUCCESS) 
                                  : static_cast<int32_t>(GuiIpcStatus::SERVICE_ERROR);
    }
    
    // ==================== 人脸识别 ====================
    
    void handle_start_recognition(GuiIpcResponse& response) {
        std::string error;
        bool success = backend_->StartRecognition(error);
        
        std::strncpy(response.payload, error.c_str(), sizeof(response.payload) - 1);
        response.payload_size = static_cast<int32_t>(error.size());
        response.status = success ? static_cast<int32_t>(GuiIpcStatus::SUCCESS) 
                                  : static_cast<int32_t>(GuiIpcStatus::SERVICE_ERROR);
    }
    
    void handle_is_recognition_running(GuiIpcResponse& response) {
        bool running = backend_->IsRecognitionRunning();
        std::strcpy(response.payload, running ? "1" : "0");
        response.payload_size = 1;
        response.status = static_cast<int32_t>(GuiIpcStatus::SUCCESS);
    }
    
    void handle_get_recognition_result(GuiIpcResponse& response) {
        auto result = backend_->GetRecognitionResult();
        
        std::ostringstream ss;
        ss << "success=" << (result.success ? "1" : "0") << "\n";
        ss << "username=" << result.username << "\n";
        ss << "confidence=" << result.confidence << "\n";
        ss << "error_message=" << result.error_message << "\n";
        
        std::string str = ss.str();
        std::strncpy(response.payload, str.c_str(), sizeof(response.payload) - 1);
        response.payload_size = static_cast<int32_t>(str.size());
        response.status = static_cast<int32_t>(GuiIpcStatus::SUCCESS);
    }

    void handle_get_recognizer_config(GuiIpcResponse& response) {
        FaceRecognizerConfig config;
        std::string error;
        if (!backend_->GetRecognizerConfig(config, error)) {
            std::strncpy(response.payload, error.c_str(), sizeof(response.payload) - 1);
            response.payload_size = static_cast<int32_t>(error.size());
            response.status = static_cast<int32_t>(GuiIpcStatus::SERVICE_ERROR);
            return;
        }

        std::ostringstream ss;
        ss << "camera=" << config.camera << "\n";
        ss << "liveness=" << (config.liveness ? "1" : "0") << "\n";
        ss << "face_threshold=" << config.face_threshold << "\n";
        ss << "liveness_threshold=" << config.liveness_threshold << "\n";
        ss << "debug=" << (config.debug ? "1" : "0") << "\n";

        const std::string result = ss.str();
        std::strncpy(response.payload, result.c_str(), sizeof(response.payload) - 1);
        response.payload_size = static_cast<int32_t>(result.size());
        response.status = static_cast<int32_t>(GuiIpcStatus::SUCCESS);
    }

    void handle_save_recognizer_config(const std::string& payload, GuiIpcResponse& response) {
        std::istringstream ss(payload);
        std::string line;
        FaceRecognizerConfig config;

        while (std::getline(ss, line)) {
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;

            const std::string key = line.substr(0, eq);
            const std::string value = line.substr(eq + 1);

            if (key == "camera") config.camera = std::stoi(value);
            else if (key == "liveness") config.liveness = (value == "1" || value == "true");
            else if (key == "face_threshold") config.face_threshold = std::stof(value);
            else if (key == "liveness_threshold") config.liveness_threshold = std::stof(value);
            else if (key == "debug") config.debug = (value == "1" || value == "true");
        }

        std::string error;
        const bool success = backend_->SaveRecognizerConfig(config, error);
        std::strncpy(response.payload, error.c_str(), sizeof(response.payload) - 1);
        response.payload_size = static_cast<int32_t>(error.size());
        response.status = success ? static_cast<int32_t>(GuiIpcStatus::SUCCESS)
                                  : static_cast<int32_t>(GuiIpcStatus::SERVICE_ERROR);
    }
};

} // namespace smile2unlock
