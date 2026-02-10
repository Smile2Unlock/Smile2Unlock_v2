// camera_win.h
#pragma once

#include <combaseapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <windows.h>
#include <algorithm>
#include <vector>
#include <iostream>
#include <thread>
#include <chrono>
#include <seeta/CStruct.h>

// 注意：此头文件设计为单翻译单元包含（推荐仅在FaceRecognizer.cpp中包含）
// 若需多文件使用，请将实现移至.cpp并移除函数体定义

// 确保Media Foundation全局初始化（线程安全，进程唯一）
inline bool EnsureMFInitialized() {
  static struct MFInitGuard {
    HRESULT hr;
    MFInitGuard() : hr(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET)) {}
    ~MFInitGuard() {
      if (SUCCEEDED(hr))
        MFShutdown();
    }
  } instance;
  return SUCCEEDED(instance.hr);
}

// 摄像头捕获类 - 支持通过索引选择摄像头设备
class CameraCapture {
 private:
  IMFMediaSource* m_mediaSource = nullptr;
  IMFSourceReader* m_reader = nullptr;
  int m_cameraIndex = 0;
  bool m_initialized = false;

  // 简单的 YUY2 to BGR24 转换函数
  static void ConvertYUY2ToBGR24(const BYTE* pYUY2Data, BYTE* pBGR24Data,
                                  UINT32 width, UINT32 height) {
    for (UINT32 y = 0; y < height; ++y) {
      const BYTE* pYUY2Row = pYUY2Data + y * width * 2;
      BYTE* pBGRRow = pBGR24Data + y * width * 3;

      for (UINT32 x = 0; x < width; x += 2) {
        // YUY2: Y0 U Y1 V
        int Y0 = pYUY2Row[x * 2];
        int U = pYUY2Row[x * 2 + 1];
        int Y1 = pYUY2Row[x * 2 + 2];
        int V = pYUY2Row[x * 2 + 3];

        // 简化的YUV到RGB转换（不完全精确但足够使用）
        int C0 = Y0 - 16;
        int C1 = Y1 - 16;
        int D = U - 128;
        int E = V - 128;

        int R0 = (298 * C0 + 409 * E + 128) >> 8;
        int G0 = (298 * C0 - 100 * D - 208 * E + 128) >> 8;
        int B0 = (298 * C0 + 516 * D + 128) >> 8;

        int R1 = (298 * C1 + 409 * E + 128) >> 8;
        int G1 = (298 * C1 - 100 * D - 208 * E + 128) >> 8;
        int B1 = (298 * C1 + 516 * D + 128) >> 8;

        // 限制范围到 0-255
        R0 = (R0 < 0) ? 0 : (R0 > 255) ? 255 : R0;
        G0 = (G0 < 0) ? 0 : (G0 > 255) ? 255 : G0;
        B0 = (B0 < 0) ? 0 : (B0 > 255) ? 255 : B0;

        R1 = (R1 < 0) ? 0 : (R1 > 255) ? 255 : R1;
        G1 = (G1 < 0) ? 0 : (G1 > 255) ? 255 : G1;
        B1 = (B1 < 0) ? 0 : (B1 > 255) ? 255 : B1;

        // 写入BGR24（Media Foundation使用BGR字节顺序）
        pBGRRow[x * 3] = B0;
        pBGRRow[x * 3 + 1] = G0;
        pBGRRow[x * 3 + 2] = R0;

        pBGRRow[x * 3 + 3] = B1;
        pBGRRow[x * 3 + 4] = G1;
        pBGRRow[x * 3 + 5] = R1;
      }
    }
  }

  // NV12 to BGR24 转换函数
  // NV12 is a YUV 4:2:0 format with Y plane followed by interleaved UV plane
  static void ConvertNV12ToBGR24(const BYTE* pNV12Data, BYTE* pBGR24Data,
                                  UINT32 width, UINT32 height) {
    const BYTE* pY = pNV12Data;  // Y plane
    const BYTE* pUV = pNV12Data + (width * height);  // UV plane starts after Y

    for (UINT32 y = 0; y < height; ++y) {
      BYTE* pBGRRow = pBGR24Data + y * width * 3;
      const BYTE* pYRow = pY + y * width;

      for (UINT32 x = 0; x < width; ++x) {
        // 获取 Y 值
        int Y = pYRow[x];

        // UV平面是4:2:0，即每2x2像素块共享一对UV值
        int uvIndex = (y / 2) * width + (x & ~1);  // x & ~1 clears the last bit, rounding down to even
        int U = pUV[uvIndex];
        int V = pUV[uvIndex + 1];

        // YUV to RGB conversion
        int C = Y - 16;
        int D = U - 128;
        int E = V - 128;

        int R = (298 * C + 409 * E + 128) >> 8;
        int G = (298 * C - 100 * D - 208 * E + 128) >> 8;
        int B = (298 * C + 516 * D + 128) >> 8;

        // 限制范围到 0-255
        R = (R < 0) ? 0 : (R > 255) ? 255 : R;
        G = (G < 0) ? 0 : (G > 255) ? 255 : G;
        B = (B < 0) ? 0 : (B > 255) ? 255 : B;

        // 写入BGR24
        pBGRRow[x * 3] = B;
        pBGRRow[x * 3 + 1] = G;
        pBGRRow[x * 3 + 2] = R;
      }
    }
  }

  // 枚举所有摄像头激活器
  HRESULT EnumerateCameras(std::vector<IMFActivate*>& activates) {
    if (!EnsureMFInitialized()) {
      std::cerr << "[Camera] Media Foundation 初始化失败" << std::endl;
      return E_FAIL;
    }

    IMFAttributes* pAttributes = nullptr;
    HRESULT hr = MFCreateAttributes(&pAttributes, 1);
    if (FAILED(hr)) {
      std::cerr << "[Camera] 创建属性对象失败: 0x" << std::hex << hr << std::endl;
      return hr;
    }

    hr = pAttributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                              MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(hr)) {
      std::cerr << "[Camera] 设置属性失败: 0x" << std::hex << hr << std::endl;
      pAttributes->Release();
      return hr;
    }

    IMFActivate** ppDevices = nullptr;
    UINT32 count = 0;
    hr = MFEnumDeviceSources(pAttributes, &ppDevices, &count);
    pAttributes->Release();

    if (FAILED(hr)) {
      std::cerr << "[Camera] 枚举摄像头失败: 0x" << std::hex << hr << std::endl;
      return hr;
    }

    std::cout << "[Camera] 检测到 " << count << " 个摄像头设备" << std::endl;

    if (SUCCEEDED(hr) && ppDevices && count > 0) {
      activates.reserve(count);
      for (UINT32 i = 0; i < count; ++i) {
        std::cout << "[Camera]   - 摄像头 " << i << " 已枚举" << std::endl;
        activates.push_back(ppDevices[i]);  // 增加引用计数由调用者管理
      }
      CoTaskMemFree(ppDevices);
    } else if (ppDevices) {
      CoTaskMemFree(ppDevices);
    }
    return hr;
  }

  HRESULT InitializeReader() {
    if (!m_mediaSource)
      return E_POINTER;

    // 创建源读取器配置，启用视频处理以支持格式转换
    IMFAttributes* pAttributes = nullptr;
    HRESULT hr = MFCreateAttributes(&pAttributes, 1);
    if (SUCCEEDED(hr)) {
      // 启用视频处理，允许格式转换
      hr = pAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
      if (FAILED(hr)) {
        std::cerr << "[Camera] 警告：无法启用视频处理: 0x" << std::hex << hr << std::endl;
      }
    }

    hr = MFCreateSourceReaderFromMediaSource(m_mediaSource, pAttributes, &m_reader);

    if (pAttributes) {
      pAttributes->Release();
    }

    if (FAILED(hr)) {
      std::cerr << "[Camera] 创建源读取器失败: 0x" << std::hex << hr << std::endl;
      return hr;
    }

    std::cout << "[Camera] 源读取器创建成功（已启用视频处理），查询支持的格式..." << std::endl;

    // 枚举摄像头支持的所有格式，找到合适的分辨率
    DWORD format_index = 0;
    IMFMediaType* pNativeType = nullptr;
    bool format_found = false;

    while (SUCCEEDED(m_reader->GetNativeMediaType(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM, format_index, &pNativeType))) {

      // 获取格式信息
      UINT32 width = 0, height = 0;
      MFGetAttributeSize(pNativeType, MF_MT_FRAME_SIZE, &width, &height);

      std::cout << "[Camera] 支持的格式 " << format_index << ": "
                << width << "x" << height << std::endl;

      // 倾向于选择 640x480 或更小的分辨率以提高兼容性
      if ((width == 640 && height == 480) ||
          (width == 320 && height == 240) ||
          (!format_found && (width * height <= 921600))) {  // <= 1280x720

        std::cout << "[Camera] 选择格式: " << width << "x" << height << std::endl;

        hr = m_reader->SetCurrentMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, pNativeType);

        if (SUCCEEDED(hr)) {
          std::cout << "[Camera] 成功设置格式: " << width << "x" << height << std::endl;
          format_found = true;
          pNativeType->Release();
          break;
        }
      }

      pNativeType->Release();
      format_index++;
    }

    if (!format_found) {
      std::cout << "[Camera] 没有找到优选格式，使用摄像头默认格式" << std::endl;
    }

    // 请求转换为 RGB24 格式（视频处理已启用）
    // 创建输出媒体类型，仅指定RGB24格式，不限制分辨率
    IMFMediaType* pOutputType = nullptr;
    hr = MFCreateMediaType(&pOutputType);
    if (SUCCEEDED(hr)) {
      pOutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
      pOutputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB24);

      hr = m_reader->SetCurrentMediaType(
          MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, pOutputType);
      pOutputType->Release();

      if (SUCCEEDED(hr)) {
        std::cout << "[Camera] 成功启用RGB24格式转换" << std::endl;
      } else {
        std::cout << "[Camera] RGB24转换不可用(0x" << std::hex << hr
                  << ")，将使用原生格式" << std::endl;
      }
    }

    return S_OK;
  }

 public:
  // 构造函数：指定摄像头索引（0=默认摄像头）
  explicit CameraCapture(int cameraIndex = 0) : m_cameraIndex(cameraIndex) {
    std::cout << "[Camera] 初始化摄像头 (索引: " << cameraIndex << ")" << std::endl;
    
    if (!EnsureMFInitialized()) {
      std::cerr << "[Camera] Media Foundation 初始化失败" << std::endl;
      m_initialized = false;
      return;
    }

    std::vector<IMFActivate*> activates;
    HRESULT hr = EnumerateCameras(activates);

    if (FAILED(hr)) {
      std::cerr << "[Camera] 摄像头枚举失败，错误码: 0x" << std::hex << hr << std::endl;
      m_initialized = false;
      return;
    }

    if (activates.empty()) {
      std::cerr << "[Camera] 错误：未找到任何摄像头设备" << std::endl;
      m_initialized = false;
      return;
    }

    if (cameraIndex < 0 || static_cast<size_t>(cameraIndex) >= activates.size()) {
      std::cerr << "[Camera] 错误：摄像头索引 " << cameraIndex << " 超出范围 (0-" 
                << (activates.size() - 1) << ")" << std::endl;
      // 清理激活器引用
      for (auto* act : activates)
        act->Release();
      m_initialized = false;
      return;
    }

    // 激活指定索引的设备
    std::cout << "[Camera] 激活摄像头 " << cameraIndex << std::endl;
    hr = activates[cameraIndex]->ActivateObject(IID_PPV_ARGS(&m_mediaSource));

    // 释放所有激活器引用
    for (auto* act : activates)
      act->Release();
    activates.clear();

    if (FAILED(hr)) {
      std::cerr << "[Camera] 激活摄像头失败，错误码: 0x" << std::hex << hr << std::endl;
      m_initialized = false;
      return;
    }

    if (!m_mediaSource) {
      std::cerr << "[Camera] 摄像头媒体源为空" << std::endl;
      m_initialized = false;
      return;
    }

    std::cout << "[Camera] 初始化读取器..." << std::endl;
    hr = InitializeReader();
    if (FAILED(hr)) {
      std::cerr << "[Camera] 初始化读取器失败，错误码: 0x" << std::hex << hr << std::endl;
      m_initialized = false;
      return;
    }

    m_initialized = true;
    std::cout << "[Camera] 摄像头初始化完成，准备捕获数据" << std::endl;
  }

  ~CameraCapture() {
    if (m_reader) {
      m_reader->Release();
      m_reader = nullptr;
    }
    if (m_mediaSource) {
      m_mediaSource->Release();
      m_mediaSource = nullptr;
    }
  }

  bool IsInitialized() const { return m_initialized; }

  // 捕获单帧并填充到SeetaImageData（调用者负责释放data）
  bool CaptureFrame(SeetaImageData& image) {
    if (!m_initialized || !m_reader) {
      std::cerr << "[Camera] 摄像头未初始化或读取器为空" << std::endl;
      return false;
    }

    // 最多尝试 10 次，等待摄像头准备好数据
    for (int attempt = 0; attempt < 10; ++attempt) {
      IMFSample* pSample = nullptr;
      DWORD streamIndex = 0;
      DWORD flags = 0;
      LONGLONG timestamp = 0;

      HRESULT hr =
          m_reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                               &streamIndex, &flags, &timestamp, &pSample);

      if (FAILED(hr)) {
        std::cerr << "[Camera] ReadSample 失败 (尝试 " << (attempt + 1) << "/10)，错误码: 0x" << std::hex << hr << std::endl;
        if (pSample)
          pSample->Release();
        if (attempt < 9) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        continue;
      }

      // 处理标志和样本检查
      if (flags & MF_SOURCE_READERF_STREAMTICK) {
        std::cout << "[Camera] 接收到 STREAMTICK，继续等待... (尝试 " << (attempt + 1) << "/10)" << std::endl;
        if (pSample)
          pSample->Release();
        if (attempt < 9) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        continue;
      }

      if (!pSample) {
        std::cerr << "[Camera] 获得 NULL 样本 (尝试 " << (attempt + 1) << "/10)" << std::endl;
        if (attempt < 9) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        continue;
      }

      // 成功获取样本，处理数据
      IMFMediaBuffer* pBuffer = nullptr;
      hr = pSample->GetBufferByIndex(0, &pBuffer);
      if (FAILED(hr)) {
        std::cerr << "[Camera] GetBufferByIndex 失败，错误码: 0x" << std::hex << hr << std::endl;
        pSample->Release();
        if (attempt < 9) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        continue;
      }

      BYTE* pData = nullptr;
      DWORD maxLength = 0;
      DWORD currentLength = 0;
      hr = pBuffer->Lock(&pData, &maxLength, &currentLength);
      if (FAILED(hr)) {
        std::cerr << "[Camera] Lock 失败，错误码: 0x" << std::hex << hr << std::endl;
        pBuffer->Release();
        pSample->Release();
        if (attempt < 9) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        continue;
      }

      // 获取帧尺寸、步长和格式
      UINT32 width = 0, height = 0;
      INT32 stride = 0;
      GUID subtype = GUID_NULL;
      bool hasStride = false;
      IMFMediaType* pCurrentType = nullptr;

      if (SUCCEEDED(m_reader->GetCurrentMediaType(
              MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pCurrentType))) {
        // 获取实际的帧尺寸
        HRESULT sizeHr = MFGetAttributeSize(pCurrentType, MF_MT_FRAME_SIZE, &width, &height);
        if (FAILED(sizeHr)) {
          std::cerr << "[Camera] 获取帧尺寸失败: 0x" << std::hex << sizeHr << std::endl;
          pCurrentType->Release();
          pBuffer->Unlock();
          pBuffer->Release();
          pSample->Release();
          continue;
        }

        // 获取格式类型
        pCurrentType->GetGUID(MF_MT_SUBTYPE, &subtype);

        // 获取步长信息
        if (SUCCEEDED(pCurrentType->GetUINT32(
                MF_MT_DEFAULT_STRIDE, reinterpret_cast<UINT32*>(&stride)))) {
          hasStride = true;
        }
        pCurrentType->Release();
      } else {
        std::cerr << "[Camera] 无法获取当前媒体类型" << std::endl;
        pBuffer->Unlock();
        pBuffer->Release();
        pSample->Release();
        continue;
      }

      // 验证获取的尺寸有效
      if (width == 0 || height == 0) {
        std::cerr << "[Camera] 获取的帧尺寸无效: " << width << "x" << height << std::endl;
        pBuffer->Unlock();
        pBuffer->Release();
        pSample->Release();
        continue;
      }

      // 分配输出缓冲区 (RGB24: 3字节/像素)
      image.width = static_cast<int>(width);
      image.height = static_cast<int>(height);
      image.channels = 3;
      image.data = new unsigned char[width * height * 3];

      // 根据格式处理数据
      if (subtype == MFVideoFormat_YUY2) {
        // YUY2 格式: 2字节/像素
        std::cout << "[Camera] 转换YUY2格式..." << std::endl;
        ConvertYUY2ToBGR24(pData, image.data, width, height);
      } else if (subtype == MFVideoFormat_RGB24) {
        // RGB24 格式: 3字节/像素 (BGR字节顺序)
        std::cout << "[Camera] 使用RGB24格式..." << std::endl;
        if (stride != 0) {
          bool isBottomUp = (stride < 0);
          int absStride = (stride < 0) ? -stride : stride;

          for (UINT32 y = 0; y < height; ++y) {
            const BYTE* srcRow =
                pData + (isBottomUp ? (height - 1 - y) : y) * absStride;
            BYTE* dstRow = image.data + y * width * 3;
            std::memcpy(dstRow, srcRow, width * 3);
          }
        } else {
          std::memcpy(image.data, pData, width * height * 3);
        }
      } else if (subtype == MFVideoFormat_RGB32) {
        // RGB32 格式: 4字节/像素，去掉alpha通道
        std::cout << "[Camera] 转换RGB32格式..." << std::endl;
        for (UINT32 i = 0; i < width * height; ++i) {
          image.data[i * 3] = pData[i * 4];      // B
          image.data[i * 3 + 1] = pData[i * 4 + 1];  // G
          image.data[i * 3 + 2] = pData[i * 4 + 2];  // R
        }
      } else if (subtype == MFVideoFormat_MJPG) {
        // MJPEG 格式需要解码 - 目前不支持
        std::cerr << "[Camera] 检测到MJPEG格式，当前不支持，请更换摄像头设置" << std::endl;
        delete[] image.data;
        image.data = nullptr;
        pBuffer->Unlock();
        pBuffer->Release();
        pSample->Release();
        continue;
      } else if (subtype == MFVideoFormat_NV12) {
        // NV12 格式 - 常见的YUV 4:2:0格式
        std::cout << "[Camera] 转换NV12格式..." << std::endl;
        ConvertNV12ToBGR24(pData, image.data, width, height);
      } else {
        // 不支持的格式 - 打印GUID以便调试
        wchar_t guidStr[40];
        StringFromGUID2(subtype, guidStr, 40);
        std::wcerr << L"[Camera] 不支持的格式 GUID: " << guidStr << std::endl;
        delete[] image.data;
        image.data = nullptr;
        pBuffer->Unlock();
        pBuffer->Release();
        pSample->Release();
        continue;
      }

      // BGR (Media Foundation) → RGB (SeetaFace) 转换
      // 所有上述格式都输出BGR，需要交换为RGB
      for (UINT32 i = 0; i < width * height; ++i) {
        std::swap(image.data[i * 3], image.data[i * 3 + 2]);
      }

      // 清理资源
      pBuffer->Unlock();
      pBuffer->Release();
      pSample->Release();

      if (image.width > 0 && image.height > 0) {
        std::cout << "[Camera] 成功捕获并转换帧为RGB24 (" << std::dec << image.width << "x" << image.height << ")" << std::endl;
        return true;
      } else {
        std::cerr << "[Camera] 帧数据验证失败" << std::endl;
        delete[] image.data;
        image.data = nullptr;
        continue;
      }
    }

    // 所有重试都失败
    std::cerr << "[Camera] 10 次尝试后仍然失败，放弃捕获" << std::endl;
    return false;
  }

  // 静态工具：获取系统可用摄像头数量
  static int GetCameraCount() {
    if (!EnsureMFInitialized())
      return 0;

    IMFAttributes* pAttributes = nullptr;
    if (FAILED(MFCreateAttributes(&pAttributes, 1)))
      return 0;

    pAttributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                         MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** ppDevices = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFEnumDeviceSources(pAttributes, &ppDevices, &count);
    pAttributes->Release();

    if (SUCCEEDED(hr) && ppDevices) {
      for (UINT32 i = 0; i < count; ++i) {
        ppDevices[i]->Release();
      }
      CoTaskMemFree(ppDevices);
    }
    return static_cast<int>(count);
  }
};
