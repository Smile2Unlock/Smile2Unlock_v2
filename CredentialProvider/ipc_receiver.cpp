#include "ipc_receiver.h"


bool ipc_receiver::receive_message(std::stop_token stoken, int id) {
    // 打开共享内存
    hMapFile = OpenFileMapping(
        FILE_MAP_ALL_ACCESS,     // 读写权限
        FALSE,                   // 不继承句柄
        SHARED_MEMORY_NAME);     // 映射对象名称

    if (hMapFile == NULL) {
        printf("打开文件映射对象失败 (%d).\n", GetLastError());
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
        printf("打开事件失败 (%d).\n", GetLastError());
        UnmapViewOfFile(pSharedData);
        CloseHandle(hMapFile);
        return 1;
    }

    printf("等待数据...\n");

    while (!stoken.stop_requested()) {
        // 等待Writer写入数据
        WaitForSingleObject(hEventDataReady, INFINITE);

        // 读取数据
        // printf("收到: %s\n", pSharedData->message);
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

    printf("Reader已退出。\n");
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