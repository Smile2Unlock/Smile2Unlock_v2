// camera.h
#pragma once

// 根据操作系统选择相应的摄像头实现
#ifdef _WIN32
    #include "camera_win.h"
#elif defined(__linux__) || defined(__unix__)
    #include "camera_linux.h"
#else
    #error "Unsupported platform"
#endif