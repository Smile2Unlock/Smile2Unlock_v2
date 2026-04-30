#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

#include <seeta/Common/CStruct.h>
#include <seeta/FaceAntiSpoofing.h>
#include <seeta/FaceDetector.h>
#include <seeta/FaceLandmarker.h>
#include <seeta/FaceRecognizer.h>

namespace su::facerecognizer {

/**
 * @brief SeetaFace 人脸识别引擎封装类
 * 
 * 封装了 SeetaFace6 SDK 的人脸检测、关键点定位、特征提取、
 * 特征比对和活体检测功能。
 */
class SeetaFaceEngine {
public:
    /**
     * @brief 构造函数
     * @param model_root 模型文件根目录，为空则使用默认路径
     */
    explicit SeetaFaceEngine(std::string model_root = {});
    
    /**
     * @brief 析构函数
     */
    ~SeetaFaceEngine();

    SeetaFaceEngine(const SeetaFaceEngine&) = delete;
    SeetaFaceEngine& operator=(const SeetaFaceEngine&) = delete;

    /**
     * @brief 检测人脸
     * @param image 输入图像
     * @return SeetaRect 第一个人脸的位置，未检测到则返回 {0, 0, 0, 0}
     */
    SeetaRect detect(const SeetaImageData& image) const;
    
    /**
     * @brief 定位人脸关键点
     * @param image 输入图像
     * @param face 人脸位置
     * @return std::vector<SeetaPointF> 关键点坐标列表（5个关键点）
     */
    std::vector<SeetaPointF> mark(const SeetaImageData& image, const SeetaRect& face) const;
    
    /**
     * @brief 提取人脸特征向量
     * @param image 输入图像
     * @param points 关键点坐标
     * @return std::vector<float> 特征向量
     */
    std::vector<float> extract(const SeetaImageData& image, std::span<const SeetaPointF> points) const;
    
    /**
     * @brief 从图像直接提取特征向量（检测+标记+提取的完整流程）
     * @param image 输入图像
     * @return std::vector<float> 特征向量，未检测到人脸则为空
     */
    std::vector<float> image_to_features(const SeetaImageData& image) const;
    
    /**
     * @brief 比较两个人脸特征向量的相似度
     * @param lhs 第一个特征向量
     * @param rhs 第二个特征向量
     * @return float 相似度得分（0.0-1.0）
     * @throw std::invalid_argument 特征维度不匹配时抛出
     */
    float compare(std::span<const float> lhs, std::span<const float> rhs) const;
    
    /**
     * @brief 活体检测
     * @param image 输入图像
     * @param liveness_threshold 活体阈值
     * @return bool true 表示真实人脸，false 表示攻击或检测失败
     */
    bool anti_spoof(const SeetaImageData& image, float liveness_threshold) const;
    
    /**
     * @brief 获取特征向量维度
     * @return int 特征向量大小
     */
    int feature_size() const;

private:
    struct Deleter {
        template <typename T>
        void operator()(T* value) const {
            delete value;
        }
    };

    std::string model_root_;
    std::unique_ptr<seeta::FaceDetector, Deleter> detector_;
    std::unique_ptr<seeta::FaceLandmarker, Deleter> landmarker_;
    std::unique_ptr<seeta::FaceRecognizer, Deleter> recognizer_;
    std::unique_ptr<seeta::FaceAntiSpoofing, Deleter> anti_spoofing_;
};

}  // namespace su::facerecognizer
