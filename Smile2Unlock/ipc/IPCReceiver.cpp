#include "IPCReceiver.h"
#include <iostream>

IPCReceiver::IPCReceiver() 
    : hMapFile(NULL)
    , pSharedData(nullptr)
    , hEventDataReady(NULL)
    , hEventDataRead(NULL)
    , initialized(false) {
}

IPCReceiver::~IPCReceiver() {
    Cleanup();
}

bool IPCReceiver::Initialize() {
    if (initialized) {
        return true;
    }

    // 打开已存在的共享内存
    hMapFile = OpenFileMapping(
        FILE_MAP_ALL_ACCESS,     // 读写权限
        FALSE,                   // 不继承句柄
        SHARED_MEMORY_NAME);     // 共享内存名称

    if (hMapFile == NULL) {
        std::cerr << "Could not open file mapping object (" << GetLastError() << ")." << std::endl;
        return false;
    }

    // 映射视图
    pSharedData = (SharedData*)MapViewOfFile(
        hMapFile,
        FILE_MAP_ALL_ACCESS,
        0,
        0,
        sizeof(SharedData));

    if (pSharedData == NULL) {
        std::cerr << "Could not map view of file (" << GetLastError() << ")." << std::endl;
        CloseHandle(hMapFile);
        hMapFile = NULL;
        return false;
    }

    // 打开事件
    hEventDataReady = OpenEvent(
        EVENT_ALL_ACCESS,
        FALSE,
        EVENT_DATA_READY_NAME);

    hEventDataRead = OpenEvent(
        EVENT_ALL_ACCESS,
        FALSE,
        EVENT_DATA_READ_NAME);

    if (hEventDataReady == NULL || hEventDataRead == NULL) {
        std::cerr << "Could not open events (" << GetLastError() << ")." << std::endl;
        Cleanup();
        return false;
    }

    initialized = true;
    return true;
}

int IPCReceiver::GetStatus(DWORD timeoutMs) {
    if (!initialized && !Initialize()) {
        return -1; // 初始化失败
    }

    // 等待数据准备好
    DWORD result = WaitForSingleObject(hEventDataReady, timeoutMs);
    
    if (result == WAIT_OBJECT_0) {
        // 读取状态
        int status = pSharedData->status;
        
        // 通知数据已读取
        SetEvent(hEventDataRead);
        
        return status;
    } else if (result == WAIT_TIMEOUT) {
        return -2; // 超时
    } else {
        return -3; // 其他错误
    }
}

std::string IPCReceiver::GetMessage() {
    if (!initialized || pSharedData == nullptr) {
        return "";
    }
    
    return std::string(pSharedData->message);
}

void IPCReceiver::Cleanup() {
    if (pSharedData != nullptr) {
        UnmapViewOfFile(pSharedData);
        pSharedData = nullptr;
    }
    
    if (hMapFile != NULL) {
        CloseHandle(hMapFile);
        hMapFile = NULL;
    }
    
    if (hEventDataReady != NULL) {
        CloseHandle(hEventDataReady);
        hEventDataReady = NULL;
    }
    
    if (hEventDataRead != NULL) {
        CloseHandle(hEventDataRead);
        hEventDataRead = NULL;
    }
    
    initialized = false;
}
