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

    while (!stoken.stop_requested())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        pSharedData->status = status_code;

        if (status_code != pSharedData->status) {
            pSharedData->newData = true;
        }
        
        SetEvent(hEventDataReady);
        //等待接收
        WaitForSingleObject(hEventDataRead, INFINITE);
    }

    // 清理资源
    UnmapViewOfFile(pSharedData);
    CloseHandle(hMapFile);
    CloseHandle(hEventDataReady);
    CloseHandle(hEventDataRead);

    return 0;
}

ipc_sender::ipc_sender() {
    // 修复：使用lambda函数正确包装成员函数调用
    threads.emplace_back([this, id = 0](std::stop_token stoken) {
        this->send_message(stoken, id);
    });
}


ipc_sender::~ipc_sender() {
    threads.clear();
}