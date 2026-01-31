#include <iostream>
#include <vector>
#include <chrono>
#include <string>
#include <windows.h>
#include <direct.h>
#include <stdio.h>
#include <type_traits>
#include <memory>
#include <stdexcept>
#include <print>
#include <opencv2/opencv.hpp>
#include <seeta/FaceDetector.h>
#include <seeta/FaceRecognizer.h>
#include <seeta/FaceLandmarker.h>
#include <seeta/FaceAntiSpoofing.h>
#include "Utils.h"

// 弱光增强：单尺度Retinex算法
cv::Mat enhanceLowLight(const cv::Mat& input);

// 自适应亮度评估
bool isLowLight(const cv::Mat& frame);

// 活体检测模型
seeta::FaceAntiSpoofing *new_fas_v2();

namespace seeta
{
    namespace cv
    {
        // using namespace ::cv;
        class ImageData : public SeetaImageData {
        public:
            ImageData( const ::cv::Mat &mat )
                : cv_mat( mat.clone() ) {
                this->width = cv_mat.cols;
                this->height = cv_mat.rows;
                this->channels = cv_mat.channels();
                this->data = cv_mat.data;
            }
        private:
            ::cv::Mat cv_mat;
        };
    }
}

class seetaface {
public:
    seetaface();
    ~seetaface();

    std::shared_ptr<float> img2features(SeetaImageData cap_img);
    void savefeat(std::shared_ptr<float> feat, const std::string data_file);
    std::shared_ptr<float> loadfeat(const std::string data_file);
    float feat_compare(std::shared_ptr<float> feat1, std::shared_ptr<float> feat2);
    bool anti_face(const SeetaImageData& image, float liveness_threshold);
    SeetaRect detect(SeetaImageData cap_img);

private:
    seeta::FaceDetector* pFD = nullptr;
    seeta::FaceLandmarker* pFL = nullptr;
    seeta::FaceRecognizer* pFR = nullptr;
    seeta::FaceAntiSpoofing* pFAS = nullptr;
    
    // 模型路径
    std::string model_path_fd;
    std::string model_path_fl;
    std::string model_path_fr;
    std::string model_path_fas1;
    std::string model_path_fas2;

    std::shared_ptr<float> extract(const SeetaImageData& image, const std::vector<SeetaPointF>& points);
    std::vector<SeetaPointF> mark(const SeetaImageData& image, const SeetaRect& face);
    bool predict(const SeetaImageData& image,
                 const SeetaRect& face,
                 const SeetaPointF* points,
                 float liveness_threshold);
};