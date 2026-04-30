module;

#include <seeta/Common/CStruct.h>
#include <tbox/coroutine/channel.h>

#include "../protocol/messages.h"
#include "../seetaface.h"

export module su.facerecognizer.pipeline;

import std;
import su.facerecognizer.camera;

export namespace su::facerecognizer {

/**
 * @brief 人脸识别流水线
 * 
 * 负责从摄像头捕获图像、检测人脸、活体检测、提取特征等完整流程。
 */
class RecognitionPipeline {
public:
    /**
     * @brief 预览帧数据结构
     */
    struct PreviewFrame {
        int width = 0;                         ///< 图像宽度
        int height = 0;                        ///< 图像高度
        int channels = 0;                      ///< 通道数
        std::vector<std::byte> image_bytes;    ///< 图像字节数据
    };

    /**
     * @brief 识别事件数据结构
     */
    struct RecognitionEvent {
        bool matched = false;                              ///< 是否匹配成功
        float similarity = 0.0f;                           ///< 相似度得分
        std::vector<float> feature;                        ///< 特征向量
        std::optional<protocol::BoundingBox> face_box;    ///< 人脸位置框
    };

    /**
     * @brief 构造函数
     * @param capture_device 图像捕获设备
     * @param engine 人脸识别引擎
     */
    RecognitionPipeline(ICaptureDevice& capture_device, SeetaFaceEngine& engine)
        : capture_device_(capture_device), engine_(engine) {}

    /**
     * @brief 请求停止流水线
     */
    void stop() {
        stop_requested_.store(true, std::memory_order_relaxed);
    }

    /**
     * @brief 检查是否已请求停止
     * @return bool 是否已请求停止
     */
    bool stopped() const {
        return stop_requested_.load(std::memory_order_relaxed);
    }

    /**
     * @brief 捕获一帧预览图像
     * 
     * 从捕获设备获取一帧图像并转换为 PreviewFrame 格式。
     * 
     * @return std::optional<PreviewFrame> 预览帧，捕获失败则返回 nullopt
     */
    std::optional<PreviewFrame> capture_preview_frame() {
        SeetaImageData image{};
        if (!capture_device_.capture_frame(image) || image.data == nullptr) {
            return std::nullopt;
        }

        PreviewFrame frame{
            .width = image.width,
            .height = image.height,
            .channels = image.channels,
            .image_bytes = std::vector<std::byte>(
                reinterpret_cast<std::byte*>(image.data),
                reinterpret_cast<std::byte*>(image.data) +
                    static_cast<std::size_t>(image.width * image.height * image.channels)),
        };
        delete[] image.data;
        return frame;
    }

    /**
     * @brief 执行一次人脸识别
     * 
     * 完整流程：捕获图像 -> 检测人脸 -> 活体检测（可选）-> 提取特征
     * 
     * @param liveness_detection 是否启用活体检测
     * @param liveness_threshold 活体检测阈值
     * @return std::optional<RecognitionEvent> 识别事件，失败则返回 nullopt
     */
    std::optional<RecognitionEvent> recognize_once(bool liveness_detection, float liveness_threshold) {
        SeetaImageData image{};
        if (!capture_device_.capture_frame(image) || image.data == nullptr) {
            return std::nullopt;
        }

        const SeetaRect face = engine_.detect(image);
        if (face.width <= 0 || face.height <= 0) {
            delete[] image.data;
            return std::nullopt;
        }

        if (liveness_detection && !engine_.anti_spoof(image, liveness_threshold)) {
            delete[] image.data;
            return std::nullopt;
        }

        auto feature = engine_.image_to_features(image);
        RecognitionEvent event{
            .matched = !feature.empty(),
            .similarity = 0.0f,
            .feature = std::move(feature),
            .face_box = protocol::BoundingBox{
                .x = face.x,
                .y = face.y,
                .width = face.width,
                .height = face.height,
            },
        };
        delete[] image.data;
        return event;
    }

private:
    ICaptureDevice& capture_device_;
    SeetaFaceEngine& engine_;
    std::atomic<bool> stop_requested_ = false;
};

}  // namespace su::facerecognizer
