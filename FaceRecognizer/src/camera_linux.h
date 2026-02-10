// camera_linux.h
#pragma once

#include <queue>
#include <thread>
#include <chrono>
#include <cstring>
#include <memory>
#include <vector>
#include <seeta/CStruct.h>

#ifdef __linux__
  #include <fcntl.h>
  #include <unistd.h>
  #include <sys/ioctl.h>
  #include <sys/mman.h>
  #include <linux/videodev2.h>
  #include <dirent.h>
  #define V4L2_AVAILABLE 1
#else
  #define V4L2_AVAILABLE 0
#endif

// 摄像头捕获类 - Linux版本使用V4L2轻量驱动
class CameraCapture {
 private:
#if V4L2_AVAILABLE
  int fd;  // 设备文件描述符
  std::vector<uint8_t> buffer;
#endif
  std::vector<std::jthread> threads;
  int m_cameraIndex;
  bool m_initialized;
  int m_width;
  int m_height;

  // V4L2初始化（仅在Linux上编译）
  bool InitV4L2Device() {
#if V4L2_AVAILABLE
    char device[32];
    snprintf(device, sizeof(device), "/dev/video%d", m_cameraIndex);
    
    fd = open(device, O_RDWR);
    if (fd < 0) {
      return false;
    }

    // 设置视频格式
    struct v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = m_width;
    fmt.fmt.pix.height = m_height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_BGR24;  // BGR24格式
    fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;

    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
      close(fd);
      fd = -1;
      return false;
    }

    // 分配缓冲区（简单版本，仅用单个缓冲）
    buffer.resize(m_width * m_height * 3);
    return true;
#else
    return false;
#endif
  }

  // 获取系统可用摄像头列表
  static int CountVideoDevices() {
#if V4L2_AVAILABLE
    int count = 0;
    DIR* dir = opendir("/dev");
    if (!dir)
      return 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
      if (strncmp(entry->d_name, "video", 5) == 0) {
        count++;
      }
    }
    closedir(dir);
    return count;
#else
    return 0;
#endif
  }

 public:
  // 构造函数：指定摄像头索引（0=默认摄像头）
  explicit CameraCapture(int cameraIndex = 0)
      : m_cameraIndex(cameraIndex), m_initialized(false),
        m_width(640), m_height(480) {
#if V4L2_AVAILABLE
    fd = -1;
    if (cameraIndex >= 0 && cameraIndex < CountVideoDevices()) {
      if (InitV4L2Device()) {
        m_initialized = true;
        
        // 启动帧捕获线程
        threads.emplace_back([this](std::stop_token stoken) {
          this->CaptureFrameThread(stoken);
        });
      }
    }
#else
    m_initialized = false;
#endif
  }

  ~CameraCapture() {
    threads.clear();
#if V4L2_AVAILABLE
    if (fd >= 0) {
      close(fd);
      fd = -1;
    }
#endif
  }

  // 检查摄像头是否已成功打开
  bool IsInitialized() const {
    return m_initialized;
  }

  // 捕获单帧并填充到SeetaImageData（调用者负责释放data）
  bool CaptureFrame(SeetaImageData& image) {
    if (!m_initialized)
      return false;

#if V4L2_AVAILABLE
    // 简单的read()方式读取帧
    int ret = read(fd, buffer.data(), buffer.size());
    if (ret < 0 || ret != static_cast<int>(buffer.size())) {
      return false;
    }

    // 分配输出缓冲区
    image.width = m_width;
    image.height = m_height;
    image.channels = 3;
    image.data = new unsigned char[m_width * m_height * 3];

    // 复制原始BGR数据
    std::memcpy(image.data, buffer.data(), buffer.size());

    // BGR (V4L2) → RGB (SeetaFace) 转换
    for (int i = 0; i < m_width * m_height; ++i) {
      std::swap(image.data[i * 3], image.data[i * 3 + 2]);
    }

    return true;
#else
    return false;
#endif
  }

  // 静态工具：获取系统可用摄像头数量
  static int GetCameraCount() {
#if V4L2_AVAILABLE
    return CountVideoDevices();
#else
    return 0;
#endif
  }

 private:
  // 后台线程：持续捕获帧
  bool CaptureFrameThread(std::stop_token stoken) {
    while (!stoken.stop_requested()) {
      SeetaImageData frame;
      if (!CaptureFrame(frame)) {
        // 释放未成功的帧
        if (frame.data) {
          delete[] frame.data;
        }
        return false;
      }
      // 这里可选：将帧放入共享队列供其他模块使用
      // frames.push(frame);
      std::this_thread::sleep_for(std::chrono::milliseconds(33));  // 约30fps
    }
    return true;
  }
};
