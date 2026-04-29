#include "seetaface.h"

import std;

namespace su::facerecognizer {

namespace {

namespace fs = std::filesystem;

seeta::ModelSetting MakeSetting(std::initializer_list<std::string> models) {
  seeta::ModelSetting setting;
  for (const auto &model : models) {
    setting.append(model.c_str());
  }
  return setting;
}

std::string ResolveModelRoot(const std::string &explicit_root) {
  if (!explicit_root.empty()) {
    return explicit_root;
  }

  const fs::path default_root = fs::current_path() / "assets" / "models";
  return default_root.string();
}

std::string RequireModel(const std::string &root, std::string_view filename) {
  const fs::path model_path = fs::path(root) / filename;
  if (!fs::exists(model_path)) {
    throw std::runtime_error("Missing model: " + model_path.string());
  }
  return model_path.string();
}

bool IsValidFace(const SeetaRect &face) {
  return face.width > 0 && face.height > 0;
}

} // namespace

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

SeetaRect SeetaFaceEngine::detect(const SeetaImageData &image) const {
  const auto faces = detector_->detect(image);
  if (faces.size == 0) {
    return SeetaRect{0, 0, 0, 0};
  }
  return faces.data[0].pos;
}

std::vector<SeetaPointF> SeetaFaceEngine::mark(const SeetaImageData &image,
                                               const SeetaRect &face) const {
  return landmarker_->mark(image, face);
}

std::vector<float>
SeetaFaceEngine::extract(const SeetaImageData &image,
                         std::span<const SeetaPointF> points) const {
  std::vector<float> feature(static_cast<std::size_t>(feature_size()));
  recognizer_->Extract(image, points.data(), feature.data());
  return feature;
}

std::vector<float>
SeetaFaceEngine::image_to_features(const SeetaImageData &image) const {
  const SeetaRect face = detect(image);
  if (!IsValidFace(face)) {
    return {};
  }
  const auto points = mark(image, face);
  return extract(image, points);
}

float SeetaFaceEngine::compare(std::span<const float> lhs,
                               std::span<const float> rhs) const {
  if (lhs.empty() || rhs.empty() || lhs.size() != rhs.size()) {
    throw std::invalid_argument("Feature dimensions do not match");
  }
  return recognizer_->CalculateSimilarity(lhs.data(), rhs.data());
}

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

int SeetaFaceEngine::feature_size() const {
  return recognizer_ ? recognizer_->GetExtractFeatureSize() : 0;
}

} // namespace su::facerecognizer
