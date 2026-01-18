#pragma once
#include <windows.h>
#include <tchar.h>
#include <string>

#define SHARED_MEMORY_NAME _T("SharedMemoryFaceRecognizer")
#define EVENT_DATA_READY_NAME _T("EventDataReady")
#define EVENT_DATA_READ_NAME _T("EventDataRead")
#define BUFFER_SIZE 256

struct SharedData {
    bool newData;
    char message[BUFFER_SIZE];
    int status;
};

class IPCReceiver {
private:
    HANDLE hMapFile;
    SharedData* pSharedData;
    HANDLE hEventDataReady;
    HANDLE hEventDataRead;
    bool initialized;

public:
    IPCReceiver();
    ~IPCReceiver();
    
    bool Initialize();
    int GetStatus(DWORD timeoutMs = 1000);
    std::string GetMessage();
    void Cleanup();
};
