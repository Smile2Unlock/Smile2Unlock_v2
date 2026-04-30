#include "seetaface.h"

import std;

namespace su::facerecognizer {

namespace {

namespace fs = std::filesystem;

/**
 * @brief 创建模型设置对象
 * @param models 模型文件路径列表
 * @return seeta::ModelSetting 模型设置对象
 */
seeta::ModelSetting MakeSetting(std::initializer_list<std::string> models) {
  seeta::ModelSetting setting;
  for (const auto &model : models) {
    setting.append(model.c_str());
  }
  return setting;
}

/**
 * @brief 解析模型根目录
 * @param explicit_root 显式指定的根目录
 * @return std::string 模型根目录路径
 */
std::string ResolveModelRoot(const std::string &explicit_root) {
  if (!explicit_root.empty()) {
    return explicit_root;
  }

  const fs::path default_root = fs::current_path() / "assets" / "models";
  return default_root.string();
}

/**
 * @brief 获取模型文件路径并验证存在性
 * @param root 模型根目录
 * @param filename 模型文件名
 * @return std::string 模型完整路径
 * @throw std::runtime_error 模型文件不存在时抛出
 */
std::string RequireModel(const std::string &root, std::string_view filename) {
  const fs::path model_path = fs::path(root) / filename;
  if (!fs::exists(model_path)) {
    throw std::runtime_error("Missing model: " + model_path.string());
  }
  return model_path.string();
}

/**
 * @brief 验证人脸位置是否有效
 * @param face 人脸位置
 * @return bool 宽高均大于0则有效
 */
bool IsValidFace(const SeetaRect &face) {
  return face.width > 0 && face.height > 0;
}

} // namespace

/**
 * @brief SeetaFaceEngine 构造函数
 * 
 * 初始化各人脸识别组件（检测器、关键点定位器、识别器、活体检测器）。
 * 活体检测器为可选，如果模型文件不存在则不初始化。
 * 
 * @param model_root 模型文件根目录
 */
SeetaFaceEngine::SeetaFaceEngine(std::string model_root)
    : model_root_(ResolveModelRoot(model_root)) {
  const auto detector_model = RequireModel(model_root_, "face_detector.csta");
  const auto landmarker_model =
      RequireModel(model_root_, "face_landmarker_pts5.csta");
  const auto recognizer_model =
      RequireModel(model_root_, "face_recognizer.csta");

  detector_ = std::unique_ptr<seeta::FaceDetector, Deleter>(
      new seeta::FaceDetector(MakeSetting({detector_model})));
  landmarker_ = std::unique_ptr<seeta::FaceLandmarker, Deleter>(
      new seeta::FaceLandmarker(MakeSetting({landmarker_model})));
  recognizer_ = std::unique_ptr<seeta::FaceRecognizer, Deleter>(
      new seeta::FaceRecognizer(MakeSetting({recognizer_model})));

  const fs::path fas_first = fs::path(model_root_) / "fas_first.csta";
  const fs::path fas_second = fs::path(model_root_) / "fas_second.csta";
  if (fs::exists(fas_first) && fs::exists(fas_second)) {
    anti_spoofing_ = std::unique_ptr<seeta::FaceAntiSpoofing, Deleter>(
        new seeta::FaceAntiSpoofing(
            MakeSetting({fas_first.string(), fas_second.string()})));
  }
}

SeetaFaceEngine::~SeetaFaceEngine() = default;

/**
 * @brief 检测图像中的人脸
 * @param image 输入图像
 * @return SeetaRect 第一个人脸的位置，未检测到则返回 {0, 0, 0, 0}
 */
SeetaRect SeetaFaceEngine::detect(const SeetaImageData &image) const {
  const auto faces = detector_->detect(image);
  if (faces.size == 0) {
    return SeetaRect{0, 0, 0, 0};
  }
  return faces.data[0].pos;
}

/**
 * @brief 定位人脸关键点
 * @param image 输入图像
 * @param face 人脸位置
 * @return std::vector<SeetaPointF> 关键点坐标列表
 */
std::vector<SeetaPointF> SeetaFaceEngine::mark(const SeetaImageData &image,
                                               const SeetaRect &face) const {
  return landmarker_->mark(image, face);
}

/**
 * @brief 提取人脸特征向量
 * @param image 输入图像
 * @param points 关键点坐标
 * @return std::vector<float> 特征向量
 */
std::vector<float>
SeetaFaceEngine::extract(const SeetaImageData &image,
                         std::span<const SeetaPointF> points) const {
  std::vector<float> feature(static_cast<std::size_t>(feature_size()));
  recognizer_->Extract(image, points.data(), feature.data());
  return feature;
}

/**
 * @brief 从图像提取特征向量的完整流程
 * 
 * 依次执行：人脸检测 -> 关键点定位 -> 特征提取
 * 
 * @param image 输入图像
 * @return std::vector<float> 特征向量，未检测到人脸则为空
 */
std::vector<float>
SeetaFaceEngine::image_to_features(const SeetaImageData &image) const {
  const SeetaRect face = detect(image);
  if (!IsValidFace(face)) {
    return {};
  }
  const auto points = mark(image, face);
  return extract(image, points);
}

/**
 * @brief 比较两个人脸特征向量的相似度
 * @param lhs 第一个特征向量
 * @param rhs 第二个特征向量
 * @return float 相似度得分
 * @throw std::invalid_argument 特征维度不匹配或为空时抛出
 */
float SeetaFaceEngine::compare(std::span<const float> lhs,
                               std::span<const float> rhs) const {
  if (lhs.empty() || rhs.empty() || lhs.size() != rhs.size()) {
    throw std::invalid_argument("Feature dimensions do not match");
  }
  return recognizer_->CalculateSimilarity(lhs.data(), rhs.data());
}

/**
 * @brief 活体检测
 * 
 * 检测人脸是否为真实人脸（非照片、视频等攻击）。
 * 如果活体检测器未初始化，则默认返回 true。
 * 
 * @param image 输入图像
 * @param liveness_threshold 活体阈值
 * @return bool true 表示真实人脸，false 表示攻击或未检测到人脸
 */
bool SeetaFaceEngine::anti_spoof(const SeetaImageData &image,
                                 float liveness_threshold) const {
  if (!anti_spoofing_) {
    return true;
  }

  const SeetaRect face = detect(image);
  if (!IsValidFace(face)) {
    return false;
  }

  const auto points = mark(image, face);
  anti_spoofing_->SetThreshold(0.3f, liveness_threshold);
  const auto status = anti_spoofing_->Predict(image, face, points.data());
  return status == seeta::FaceAntiSpoofing::REAL;
}

/**
 * @brief 获取特征向量维度
 * @return int 特征向量大小，识别器未初始化则返回 0
 */
int SeetaFaceEngine::feature_size() const {
  return recognizer_ ? recognizer_->GetExtractFeatureSize() : 0;
}

} // namespace su::facerecognizer
