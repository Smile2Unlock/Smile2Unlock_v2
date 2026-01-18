#pragma once
#include <windows.h>
#include <stdio.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <tchar.h>

#define SHARED_MEMORY_NAME _T("SharedMemoryFaceRecognizer")
#define EVENT_DATA_READY_NAME _T("EventDataReady")
#define EVENT_DATA_READ_NAME _T("EventDataRead")
#define BUFFER_SIZE 256

// 全局状态码，用于在模块间共享状态
inline int status_code = 0;

struct SharedData {
    bool newData;
    char message[BUFFER_SIZE] = "Reserved interface for future use";
    int status;
};

class IPCSender {
private:
    HANDLE hMapFile;
    SharedData* pSharedData;
    HANDLE hEventDataReady;
    HANDLE hEventDataRead;
    std::vector<std::jthread> threads;

public:
    IPCSender();
    ~IPCSender();
    
    bool send_message(std::stop_token stoken, int id);
};
