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

// YUV到RGB转换查找表 - 高精度预计算，避免运行时整数精度损失
class YUVtoRGBLookupTable {
 private:
  // 查找表：Y[256], U/V[256] -> RGB分量调整值
  int m_RV[256];  // R = Y + RV[V]
  int m_GU[256];  // G = Y + GU[U] + GV[V]
  int m_GV[256];
  int m_BU[256];  // B = Y + BU[U]
  int m_Y[256];   // 调整后的Y值

 public:
  YUVtoRGBLookupTable() {
    // 使用 BT.601 标准（适用于SD视频，摄像头常用）
    // RGB = [1.164*(Y-16) + coeffs*(U/V-128)]
    for (int i = 0; i < 256; ++i) {
      // Y 分量调整（乘以1.164并左移8位以保持精度）
      m_Y[i] = (int)((1.164 * (i - 16)) * 256 + 0.5);

      // U/V 调整值（左移8位保持精度）
      int uv_offset = i - 128;
      m_RV[i] = (int)(1.596 * uv_offset * 256 + 0.5);
      m_GU[i] = (int)(-0.391 * uv_offset * 256 + 0.5);
      m_GV[i] = (int)(-0.813 * uv_offset * 256 + 0.5);
      m_BU[i] = (int)(2.018 * uv_offset * 256 + 0.5);
    }
  }

  // 内联函数：快速转换单个像素（输出RGB顺序）
  inline void YUVtoRGB(int Y, int U, int V, BYTE& R, BYTE& G, BYTE& B) const {
    int y_val = m_Y[Y];
    int r = (y_val + m_RV[V]) >> 8;
    int g = (y_val + m_GU[U] + m_GV[V]) >> 8;
    int b = (y_val + m_BU[U]) >> 8;

    // 快速裁剪到 [0, 255]
    R = (r < 0) ? 0 : (r > 255) ? 255 : (BYTE)r;
    G = (g < 0) ? 0 : (g > 255) ? 255 : (BYTE)g;
    B = (b < 0) ? 0 : (b > 255) ? 255 : (BYTE)b;
  }
};

// 摄像头捕获类 - 支持通过索引选择摄像头设备
class CameraCapture {
 private:
  IMFMediaSource* m_mediaSource = nullptr;
  IMFSourceReader* m_reader = nullptr;
  int m_cameraIndex = 0;
  bool m_initialized = false;

  // YUV转换查找表（单例，所有实例共享）
  static const YUVtoRGBLookupTable& GetYUVLUT() {
    static YUVtoRGBLookupTable lut;
    return lut;
  }

  // 改进的 YUY2 to RGB24 转换函数（直接输出RGB，不再是BGR）
  static void ConvertYUY2ToRGB24(const BYTE* pYUY2Data, BYTE* pRGB24Data,
                                  UINT32 width, UINT32 height) {
    const YUVtoRGBLookupTable& lut = GetYUVLUT();

    for (UINT32 y = 0; y < height; ++y) {
      const BYTE* pYUY2Row = pYUY2Data + y * width * 2;
      BYTE* pRGBRow = pRGB24Data + y * width * 3;

      for (UINT32 x = 0; x < width; x += 2) {
        // YUY2 格式: Y0 U Y1 V（每两个像素共享 U/V）
        int Y0 = pYUY2Row[x * 2];
        int U = pYUY2Row[x * 2 + 1];
        int Y1 = pYUY2Row[x * 2 + 2];
        int V = pYUY2Row[x * 2 + 3];

        BYTE R0, G0, B0, R1, G1, B1;
        lut.YUVtoRGB(Y0, U, V, R0, G0, B0);
        lut.YUVtoRGB(Y1, U, V, R1, G1, B1);

        // 直接写入 RGB 顺序（不再是BGR）
        pRGBRow[x * 3] = R0;
        pRGBRow[x * 3 + 1] = G0;
        pRGBRow[x * 3 + 2] = B0;

        pRGBRow[x * 3 + 3] = R1;
        pRGBRow[x * 3 + 4] = G1;
        pRGBRow[x * 3 + 5] = B1;
      }
    }
  }

  // 改进的 NV12 to RGB24 转换函数（直接输出RGB，不再是BGR）
  // NV12 is a YUV 4:2:0 format with Y plane followed by interleaved UV plane
  static void ConvertNV12ToRGB24(const BYTE* pNV12Data, BYTE* pRGB24Data,
                                  UINT32 width, UINT32 height) {
    const YUVtoRGBLookupTable& lut = GetYUVLUT();
    const BYTE* pY = pNV12Data;  // Y plane
    const BYTE* pUV = pNV12Data + (width * height);  // UV plane starts after Y

    for (UINT32 y = 0; y < height; ++y) {
      BYTE* pRGBRow = pRGB24Data + y * width * 3;
      const BYTE* pYRow = pY + y * width;

      for (UINT32 x = 0; x < width; ++x) {
        // 获取 Y 值
        int Y = pYRow[x];

        // UV平面是4:2:0，即每2x2像素块共享一对UV值
        int uvIndex = (y / 2) * width + (x & ~1);  // x & ~1 向下取偶数
        int U = pUV[uvIndex];
        int V = pUV[uvIndex + 1];

        BYTE R, G, B;
        lut.YUVtoRGB(Y, U, V, R, G, B);

        // 直接写入 RGB 顺序（不再是BGR）
        pRGBRow[x * 3] = R;
        pRGBRow[x * 3 + 1] = G;
        pRGBRow[x * 3 + 2] = B;
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
        std::cout << "[Camera] 转换YUY2格式为RGB..." << std::endl;
        ConvertYUY2ToRGB24(pData, image.data, width, height);
      } else if (subtype == MFVideoFormat_RGB24) {
        // RGB24 格式: 3字节/像素
        // Media Foundation 中的 RGB24 通常是 BGR 顺序的 bottom-up 格式
        std::cout << "[Camera] 处理RGB24格式..." << std::endl;
        if (stride != 0) {
          bool isBottomUp = (stride < 0);
          int absStride = (stride < 0) ? -stride : stride;

          for (UINT32 y = 0; y < height; ++y) {
            const BYTE* srcRow =
                pData + (isBottomUp ? (height - 1 - y) : y) * absStride;
            BYTE* dstRow = image.data + y * width * 3;

            // 复制并交换 BGR→RGB（因为 Media Foundation RGB24 是 BGR 顺序）
            for (UINT32 x = 0; x < width; ++x) {
              dstRow[x * 3] = srcRow[x * 3 + 2];      // R (from B position)
              dstRow[x * 3 + 1] = srcRow[x * 3 + 1];  // G
              dstRow[x * 3 + 2] = srcRow[x * 3];      // B (from R position)
            }
          }
        } else {
          // stride 为 0 时，假设是连续存储
          for (UINT32 i = 0; i < width * height; ++i) {
            image.data[i * 3] = pData[i * 3 + 2];      // BGR→RGB
            image.data[i * 3 + 1] = pData[i * 3 + 1];
            image.data[i * 3 + 2] = pData[i * 3];
          }
        }
      } else if (subtype == MFVideoFormat_RGB32) {
        // RGB32 格式: 4字节/像素，去掉alpha通道
        // 同样需要处理 BGR 到 RGB 的转换
        std::cout << "[Camera] 转换RGB32格式为RGB24..." << std::endl;
        for (UINT32 i = 0; i < width * height; ++i) {
          image.data[i * 3] = pData[i * 4 + 2];      // BGR32→RGB24
          image.data[i * 3 + 1] = pData[i * 4 + 1];
          image.data[i * 3 + 2] = pData[i * 4];
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
        std::cout << "[Camera] 转换NV12格式为RGB..." << std::endl;
        ConvertNV12ToRGB24(pData, image.data, width, height);
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

      // 注意：所有格式现在都输出 RGB 顺序，无需再进行全局交换

      // 清理资源
      pBuffer->Unlock();
      pBuffer->Release();
      pSample->Release();

      if (image.width > 0 && image.height > 0) {
        std::cout << "[Camera] 成功捕获并转换帧为RGB24 (" << std::dec << image.width << "x" << image.height << ")" << std::endl;

        // 首次捕获时，执行色彩验证
        static bool firstCapture = true;
        if (firstCapture) {
          std::cout << "[Camera] 执行首帧色彩验证..." << std::endl;
          ValidateColorStatistics(image);
          firstCapture = false;
        }

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

  // 色彩验证：计算图像的RGB统计信息
  static void ValidateColorStatistics(const SeetaImageData& image) {
    if (!image.data || image.width <= 0 || image.height <= 0) {
      std::cerr << "[Camera] 无效的图像数据" << std::endl;
      return;
    }

    long long sumR = 0, sumG = 0, sumB = 0;
    int minR = 255, minG = 255, minB = 255;
    int maxR = 0, maxG = 0, maxB = 0;
    int totalPixels = image.width * image.height;

    for (int i = 0; i < totalPixels; ++i) {
      int R = image.data[i * 3];
      int G = image.data[i * 3 + 1];
      int B = image.data[i * 3 + 2];

      sumR += R; sumG += G; sumB += B;
      minR = (R < minR) ? R : minR;
      minG = (G < minG) ? G : minG;
      minB = (B < minB) ? B : minB;
      maxR = (R > maxR) ? R : maxR;
      maxG = (G > maxG) ? G : maxG;
      maxB = (B > maxB) ? B : maxB;
    }

    double avgR = (double)sumR / totalPixels;
    double avgG = (double)sumG / totalPixels;
    double avgB = (double)sumB / totalPixels;

    std::cout << "[Camera] 色彩统计 (" << image.width << "x" << image.height << "):" << std::endl;
    std::cout << "  R: 平均=" << avgR << " 范围=[" << minR << ", " << maxR << "]" << std::endl;
    std::cout << "  G: 平均=" << avgG << " 范围=[" << minG << ", " << maxG << "]" << std::endl;
    std::cout << "  B: 平均=" << avgB << " 范围=[" << minB << ", " << maxB << "]" << std::endl;

    // 异常检测：如果蓝色明显偏高，可能颜色顺序错误
    if (avgB > avgR * 1.3 && avgB > avgG * 1.3) {
      std::cerr << "  [警告] 蓝色分量异常偏高，可能存在BGR/RGB顺序错误" << std::endl;
    }
    // 检测是否接近灰度图（可能是格式转换问题）
    double colorDiff = std::abs(avgR - avgG) + std::abs(avgG - avgB) + std::abs(avgB - avgR);
    if (colorDiff < 10.0) {
      std::cerr << "  [警告] 颜色差异极小，可能丢失色彩信息" << std::endl;
    }
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
