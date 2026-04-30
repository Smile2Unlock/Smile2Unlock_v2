module;

#include <seeta/Common/CStruct.h>

export module su.facerecognizer.camera;

import std;

export namespace su::facerecognizer {

/**
 * @brief 捕获设备接口
 * 
 * 定义了摄像头或其他图像捕获设备的抽象接口。
 */
class ICaptureDevice {
public:
    virtual ~ICaptureDevice() = default;
    
    /**
     * @brief 检查设备是否已初始化
     * @return bool 是否已初始化
     */
    virtual bool is_initialized() const = 0;
    
    /**
     * @brief 捕获一帧图像
     * @param image 输出参数，捕获的图像数据
     * @return bool 是否成功捕获
     */
    virtual bool capture_frame(SeetaImageData& image) = 0;
};

/**
 * @brief 摄像头捕获类
 * 
 * 实现 ICaptureDevice 接口，用于从摄像头捕获图像。
 * 当前为占位实现，实际应集成 OpenCV 或其他摄像头库。
 */
class CameraCapture final : public ICaptureDevice {
public:
    /**
     * @brief 构造函数
     * @param camera_index 摄像头索引，默认为 0
     */
    explicit CameraCapture(int camera_index = 0) : camera_index_(camera_index) {}
    ~CameraCapture() override = default;

    /**
     * @brief 检查摄像头是否已初始化
     * @return bool 摄像头索引 >= 0 则认为已初始化
     */
    bool is_initialized() const override {
        return camera_index_ >= 0;
    }

    /**
     * @brief 捕获一帧图像
     * 
     * 当前为占位实现，返回空图像。
     * 
     * @param image 输出参数，捕获的图像数据
     * @return bool 当前始终返回 false
     */
    bool capture_frame(SeetaImageData& image) override {
        image.width = 0;
        image.height = 0;
        image.channels = 0;
        image.data = nullptr;
        return false;
    }

    /**
     * @brief 枚举可用的摄像头
     * 
     * 当前为占位实现，返回一个虚拟摄像头名称。
     * 
     * @return std::vector<std::string> 摄像头名称列表
     */
    static std::vector<std::string> enumerate_cameras() {
        return {"camera#0"};
    }

private:
    int camera_index_ = 0;
};

}  // namespace su::facerecognizer
