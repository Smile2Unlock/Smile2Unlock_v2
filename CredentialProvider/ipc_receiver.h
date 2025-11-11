#include <windows.h>
#include <stdio.h>
#include <tchar.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>

#define SHARED_MEMORY_NAME _T("SharedMemoryFaceRecognizer")
#define EVENT_DATA_READY_NAME _T("EventDataReady")
#define EVENT_DATA_READ_NAME _T("EventDataRead")
#define BUFFER_SIZE 256

inline int face_reconizer_status = 0;

struct SharedData {
    bool newData;
    char message[BUFFER_SIZE] = "Reserved interface for future use";
    int status;
};

class ipc_receiver {
private:
    HANDLE hMapFile;
    SharedData* pSharedData;
    HANDLE hEventDataReady, hEventDataRead;
    std::vector<std::jthread> threads;
public:
    ipc_receiver();
    ~ipc_receiver();
    bool receive_message(std::stop_token stoken, int id);
};