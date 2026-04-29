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

class SeetaFaceEngine {
public:
    explicit SeetaFaceEngine(std::string model_root = {});
    ~SeetaFaceEngine();

    SeetaFaceEngine(const SeetaFaceEngine&) = delete;
    SeetaFaceEngine& operator=(const SeetaFaceEngine&) = delete;

    SeetaRect detect(const SeetaImageData& image) const;
    std::vector<SeetaPointF> mark(const SeetaImageData& image, const SeetaRect& face) const;
    std::vector<float> extract(const SeetaImageData& image, std::span<const SeetaPointF> points) const;
    std::vector<float> image_to_features(const SeetaImageData& image) const;
    float compare(std::span<const float> lhs, std::span<const float> rhs) const;
    bool anti_spoof(const SeetaImageData& image, float liveness_threshold) const;
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
