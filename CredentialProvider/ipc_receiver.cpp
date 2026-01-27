#include "ipc_receiver.h"


bool ipc_receiver::receive_message(std::stop_token stoken, int id) {
    printf("[IPC] receive_message线程已启动\n");
    OutputDebugStringW(L"[IPC] receive_message线程已启动\n");
    
    // 重试逻辑：等待共享内存被创建
    int retryCount = 0;
    const int maxRetries = 100; // 最多重试100次，共10秒
    
    while (retryCount < maxRetries && hMapFile == NULL) {
        printf("[IPC] 尝试打开共享内存（第%d次），超时剩余: %ds\n", 
               retryCount + 1, (maxRetries - retryCount) / 10);
        
        // 打开共享内存
        hMapFile = OpenFileMapping(
            FILE_MAP_ALL_ACCESS,     // 读写权限
            FALSE,                   // 不继承句柄
            SHARED_MEMORY_NAME);     // 映射对象名称

        if (hMapFile == NULL) {
            DWORD err = GetLastError();
            printf("[IPC] 打开文件映射对象失败 (%d), 等待FaceRecognizer创建...\n", err);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            retryCount++;
        } else {
            printf("[IPC] 文件映射对象打开成功\n");
            break;
        }
    }
    
    if (hMapFile == NULL) {
        printf("[IPC] 无法打开共享内存，超时退出\n");
        OutputDebugStringW(L"[IPC] 无法打开共享内存，超时\n");
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
        printf("[IPC] 映射视图失败 (%d).\n", GetLastError());
        CloseHandle(hMapFile);
        return 1;
    }
    printf("[IPC] 映射视图成功\n");

    // 重试逻辑：等待事件被创建
    retryCount = 0;
    while (retryCount < maxRetries && (hEventDataReady == NULL || hEventDataRead == NULL)) {
        printf("[IPC] 尝试打开事件（第%d次）\n", retryCount + 1);
        
        // 打开事件
        hEventDataReady = OpenEvent(
            SYNCHRONIZE | EVENT_MODIFY_STATE,  // 需要等待和设置事件的权限
            FALSE,
            EVENT_DATA_READY_NAME);

        hEventDataRead = OpenEvent(
            SYNCHRONIZE | EVENT_MODIFY_STATE,  // 需要等待和设置事件的权限
            FALSE,
            EVENT_DATA_READ_NAME);

        if (hEventDataReady == NULL || hEventDataRead == NULL) {
            printf("[IPC] 打开事件失败 (%d), hEventDataReady=%p, hEventDataRead=%p, 等待FaceRecognizer创建...\n", 
                   GetLastError(), hEventDataReady, hEventDataRead);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            retryCount++;
        } else {
            printf("[IPC] 事件打开成功\n");
            break;
        }
    }

    if (hEventDataReady == NULL || hEventDataRead == NULL) {
        printf("[IPC] 无法打开事件，超时退出\n");
        OutputDebugStringW(L"[IPC] 无法打开事件，超时\n");
        UnmapViewOfFile(pSharedData);
        CloseHandle(hMapFile);
        return 1;
    }

    printf("[IPC] IPC连接已建立，开始等待数据...\n");
    OutputDebugStringW(L"[IPC] IPC连接已建立，开始等待数据\n");

    int receiveCount = 0;
    int lastStatus = 0;
    
    while (!stoken.stop_requested()) {
        // 等待Writer写入数据
        DWORD waitResult = WaitForSingleObject(hEventDataReady, 1000); // 1秒超时
        
        if (waitResult == WAIT_TIMEOUT) {
            // printf("[IPC] 等待超时，继续等待...\n");
            continue;
        } else if (waitResult != WAIT_OBJECT_0) {
            printf("[IPC] WaitForSingleObject失败: %d\n", waitResult);
            break;
        }

        // 读取数据
        receiveCount++;
        int currentStatus = pSharedData->status;
        
        // 只在状态变化时输出日志
        if (currentStatus != lastStatus) {
            printf("[IPC] 收到第 %d 条消息，状态码已更新: %d -> %d\n", 
                   receiveCount, lastStatus, currentStatus);
            OutputDebugStringW(L"[IPC] 状态码已更新\n");
            lastStatus = currentStatus;
        } else {
            printf("[IPC] 收到心跳，状态码: %d\n", currentStatus);
        }
        
        face_reconizer_status = pSharedData->status;

        // 标记数据已读取
        pSharedData->newData = false;

        // 通知Writer数据已读取
        SetEvent(hEventDataRead);

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 清理资源
    UnmapViewOfFile(pSharedData);
    CloseHandle(hMapFile);
    CloseHandle(hEventDataReady);
    CloseHandle(hEventDataRead);

    printf("[IPC] Reader已退出，共接收 %d 条消息。\n", receiveCount);
    return 0;
}

ipc_receiver::ipc_receiver() {
    threads.emplace_back([this, id = 0](std::stop_token stoken) {
        this->receive_message(stoken, id);
    });
}

ipc_receiver::~ipc_receiver() {
    threads.clear();
}