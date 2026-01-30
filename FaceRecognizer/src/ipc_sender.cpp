#pragma once
#include "ipc_sender.h"


bool ipc_sender::send_message(std::stop_token stoken,int id) {
    // 创建共享内存
    hMapFile = CreateFileMapping(
        INVALID_HANDLE_VALUE,    // 使用分页文件
        NULL,                    // 默认安全属性
        PAGE_READWRITE,          // 可读写
        0,                       // 最大对象大小的高位
        sizeof(SharedData),      // 最大对象大小的低位
        SHARED_MEMORY_NAME);     // 映射对象名称

    if (hMapFile == NULL) {
        printf("创建文件映射对象失败 (%d).\n", GetLastError());
        return 1;
    }

    // 将文件映射对象映射到进程的地址空间
    pSharedData = (SharedData*)MapViewOfFile(
        hMapFile,                // 文件映射对象句柄
        FILE_MAP_ALL_ACCESS,     // 读写权限
        0,
        0,
        sizeof(SharedData));

    if (pSharedData == NULL) {
        printf("映射视图失败 (%d).\n", GetLastError());
        CloseHandle(hMapFile);
        return 1;
    }

    // 创建事件
    hEventDataReady = CreateEvent(
        NULL,                    // 默认安全属性
        FALSE,                   // 自动重置事件
        FALSE,                   // 初始状态为无信号
        EVENT_DATA_READY_NAME);  // 事件名称

    hEventDataRead = CreateEvent(
        NULL,                    // 默认安全属性
        FALSE,                   // 自动重置事件
        FALSE,                   // 初始状态为无信号
        EVENT_DATA_READ_NAME);   // 事件名称

    if (hEventDataReady == NULL || hEventDataRead == NULL) {
        printf("创建事件失败 (%d).\n", GetLastError());
        UnmapViewOfFile(pSharedData);
        CloseHandle(hMapFile);
        return 1;
    }

    printf("[IPC Sender] 开始发送循环，状态码初始值: %d\n", status_code);
    
    while (!stoken.stop_requested())
    {
        // 立即设置当前状态码
        int prev_status = pSharedData->status;
        pSharedData->status = status_code;
        
        // 只在状态码变化时输出日志
        if (prev_status != status_code) {
            printf("[IPC Sender] 状态码已更新: %d -> %d\n", prev_status, status_code);
            pSharedData->newData = true;
        }
        
        // 立即触发事件
        SetEvent(hEventDataReady);
        printf("[IPC Sender] 已发送事件信号，状态码: %d\n", pSharedData->status);
        
        // 等待接收方确认（带超时避免死锁）
        DWORD waitResult = WaitForSingleObject(hEventDataRead, 1000);
        if (waitResult == WAIT_TIMEOUT) {
            printf("[IPC Sender] 等待接收方确认超时\n");
        }
        
        // 减少睡眠时间以提高响应速度
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 清理资源
    UnmapViewOfFile(pSharedData);
    CloseHandle(hMapFile);
    CloseHandle(hEventDataReady);
    CloseHandle(hEventDataRead);

    return 0;
}

ipc_sender::ipc_sender() {
    printf("[IPC Sender] 构造函数被调用，创建发送线程\n");
    // 修复：使用lambda函数正确包装成员函数调用
    threads.emplace_back([this, id = 0](std::stop_token stoken) {
        printf("[IPC Sender] 发送线程已启动\n");
        this->send_message(stoken, id);
    });
}


ipc_sender::~ipc_sender() {
    threads.clear();
}