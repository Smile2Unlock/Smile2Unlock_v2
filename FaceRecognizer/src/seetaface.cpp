#include "seetaface.h"

// 通用模型创建函数
template <typename T>
T* create_seeta_model(const std::string& model_path) {
    seeta::ModelSetting setting;
    setting.append(model_path.c_str());
    return new T(setting);
}

seeta::FaceAntiSpoofing *new_fas_v2() {
    seeta::ModelSetting setting;
    setting.append(Utils::getCurrentDirectory() + "\\resources\\models\\" + "fas_first.csta");
    setting.append(Utils::getCurrentDirectory() + "\\resources\\models\\" + "fas_second.csta");
    return new seeta::FaceAntiSpoofing(setting);
}

bool isLowLight(const cv::Mat& frame) {
    cv::Mat gray;
    cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
    
    cv::Scalar mean, stddev;
    cv::meanStdDev(gray, mean, stddev);
    
    // 如果平均亮度低于阈值，则判定为弱光环境
    return mean[0] < 70;
}

cv::Mat enhanceLowLight(const cv::Mat& input) {
    cv::Mat src;
    input.convertTo(src, CV_32F);
    
    // 高斯模糊参数
    int gaussianSize = 15;
    double sigma = 30.0;
    
    // 分离通道
    std::vector<cv::Mat> channels;
    cv::split(src, channels);
    
    // 对每个通道应用Retinex
    for (int i = 0; i < 3; i++) {
        cv::Mat gaussian;
        cv::GaussianBlur(channels[i], gaussian, cv::Size(gaussianSize, gaussianSize), sigma);
        
        // 避免除零
        cv::Mat minVals;
        cv::min(gaussian, 0.01, minVals);
        cv::max(gaussian, minVals, gaussian);
        
        // Retinex公式: log(I) - log(G*I)
        cv::Mat logInput, logGaussian;
        cv::log(channels[i] + 1, logInput);
        cv::log(gaussian + 1, logGaussian);
        channels[i] = logInput - logGaussian;
    }
    
    // 合并通道
    cv::Mat result;
    cv::merge(channels, result);
    
    // 归一化到0-255
    cv::normalize(result, result, 0, 255, cv::NORM_MINMAX);
    result.convertTo(result, CV_8UC3);
    
    return result;
}

seetaface::seetaface() {
    std::cout << "[Recognizer] 初始化中..." << std::endl;

    // 初始化模型路径字符串 (避免悬空指针)
    std::string project_root = Utils::getCurrentDirectory(); // 使用 getCurrentDirectory 替代 GetProjectRoot
    model_path_fd = project_root + "\\resources\\models\\face_detector.csta";
    model_path_fl = project_root + "\\resources\\models\\face_landmarker_pts5.csta";
    model_path_fr = project_root + "\\resources\\models\\face_recognizer.csta";
    model_path_fas1 = project_root + "\\resources\\models\\fas_first.csta";
    model_path_fas2 = project_root + "\\resources\\models\\fas_second.csta";

    try {
        // 检查模型文件是否存在
        if (!std::filesystem::exists(model_path_fd)) {
            throw std::runtime_error("人脸检测模型文件不存在: " + model_path_fd);
        }
        if (!std::filesystem::exists(model_path_fl)) {
            throw std::runtime_error("人脸特征点定位模型文件不存在: " + model_path_fl);
        }
        if (!std::filesystem::exists(model_path_fr)) {
            throw std::runtime_error("人脸识别模型文件不存在: " + model_path_fr);
        }
        if (!std::filesystem::exists(model_path_fas1) || !std::filesystem::exists(model_path_fas2)) {
            std::cout << "[Recognizer] 警告: 活体检测模型文件不存在，将禁用活体检测功能" << std::endl;
        }

        // 加载人脸检测器
        seeta::ModelSetting setting_fd;
        setting_fd.append(model_path_fd.c_str());
        pFD = new seeta::FaceDetector(setting_fd);

        // 加载特征点定位器
        seeta::ModelSetting setting_fl;
        setting_fl.append(model_path_fl.c_str());
        pFL = new seeta::FaceLandmarker(setting_fl);

        // 加载人脸识别器
        seeta::ModelSetting setting_fr;
        const char* models[] = { model_path_fr.c_str(), NULL };
        setting_fr.model = models;
        pFR = new seeta::FaceRecognizer(setting_fr);

        // 加载活体检测器（如果文件存在）
        if (std::filesystem::exists(model_path_fas1) && std::filesystem::exists(model_path_fas2)) {
            seeta::ModelSetting setting_fas;
            setting_fas.append(model_path_fas1);
            setting_fas.append(model_path_fas2);
            pFAS = new seeta::FaceAntiSpoofing(setting_fas);
        } else {
            pFAS = nullptr;
        }

        std::cout << "[Recognizer] 初始化完成" << std::endl;
    }
    catch (const std::exception& e) {
        std::cerr << "[Recognizer] 初始化失败: " << e.what() << std::endl;
        if (pFD) delete pFD;
        if (pFL) delete pFL;
        if (pFR) delete pFR;
        if (pFAS) delete pFAS;
        throw;
    }
}

seetaface::~seetaface() {
    delete pFD;
    delete pFL;
    delete pFR;
    delete pFAS;
}

/**
 * @brief 将图像转换为人脸特征向量
 * 
 * 该函数执行完整的人脸识别流程：
 * 1. 检测图像中的人脸位置
 * 2. 标记人脸关键点
 * 3. 提取人脸特征
 * 
 * @param cap_img 输入的图像数据
 * @return std::shared_ptr<float> 人脸特征向量，如果检测失败返回nullptr
 */
std::shared_ptr<float> seetaface::img2features(SeetaImageData cap_img) {
    // 检测人脸位置
    SeetaRect pos = detect(cap_img);
    
    // 检查人脸检测是否成功，如果返回的矩形宽高为0表示检测失败
    if (pos.width == 0 || pos.height == 0) return nullptr;
    
    // 标记人脸关键点
    auto points = mark(cap_img, pos);
    
    // 提取人脸特征向量
    return extract(cap_img, points);
}

/**
 * @brief 保存人脸特征到文件
 * 
 * @param feat 人脸特征向量
 * @param data_file 保存文件路径
 */
void seetaface::savefeat(std::shared_ptr<float> feat, const std::string data_file) {
    // 获取特征向量大小
    int size = pFR->GetExtractFeatureSize();
    // 将特征向量以二进制格式保存到文件
    Utils::SaveFeaturesBinary(feat, size, data_file);
}

/**
 * @brief 从文件加载人脸特征
 * 
 * @param data_file 特征文件路径
 * @return std::shared_ptr<float> 加载的人脸特征向量
 */
std::shared_ptr<float> seetaface::loadfeat(const std::string data_file) {
    // 获取特征向量大小
    int size = pFR->GetExtractFeatureSize();
    // 从二进制文件加载特征向量
    return Utils::LoadFeaturesBinary(data_file, size);
}

/**
 * @brief 比较两个人脸特征的相似度
 * 
 * @param feat1 第一个特征向量
 * @param feat2 第二个特征向量
 * @return float 相似度分数，值越大表示越相似
 */
float seetaface::feat_compare(std::shared_ptr<float> feat1, std::shared_ptr<float> feat2) {
    // 使用SeetaFace识别器计算两个特征向量的相似度
    return pFR->CalculateSimilarity(feat1.get(), feat2.get());
}

/**
 * @brief 从图像和关键点提取人脸特征
 * 
 * @param image 输入图像
 * @param points 人脸关键点
 * @return std::shared_ptr<float> 提取的人脸特征向量
 */
std::shared_ptr<float> seetaface::extract(const SeetaImageData& image, const std::vector<SeetaPointF>& points) {
    // 创建特征向量的智能指针，使用自定义删除器确保数组正确释放
    std::shared_ptr<float> features(
        new float[pFR->GetExtractFeatureSize()], 
        [](float* p) { delete[] p; }  // 自定义删除器，使用delete[]释放数组
    );
    
    // 调用SeetaFace识别器提取特征
    pFR->Extract(image, points.data(), features.get());
    
    return features;
}

/**
 * @brief 标记人脸关键点
 * 
 * @param image 输入图像
 * @param face 人脸位置矩形
 * @return std::vector<SeetaPointF> 人脸关键点集合
 */
std::vector<SeetaPointF> seetaface::mark(const SeetaImageData& image, const SeetaRect& face) {
    // 使用SeetaFace标记器获取人脸关键点
    return pFL->mark(image, face);
}

/**
 * @brief 检测图像中的人脸
 * 
 * @param cap_img 输入图像
 * @return SeetaRect 检测到的人脸位置，如果没有检测到人脸返回{0,0,0,0}
 */
SeetaRect seetaface::detect(SeetaImageData cap_img) {
    try {
        // 使用SeetaFace检测器检测图像中的人脸
        SeetaFaceInfoArray faces = pFD->detect(cap_img);
        
        // 检查是否检测到人脸
        if (faces.size == 0) throw std::runtime_error("No face detected");
        
        // 返回第一个检测到的人脸位置
        return faces.data[0].pos;
    } catch (const std::exception& e) {
        // 捕获并打印异常信息
        printf("Error: %s\n", e.what());
        // 返回无效矩形表示检测失败
        return SeetaRect{0, 0, 0, 0};
    }
}

/**
 * @brief 预测人脸是否为真实人脸（活体检测）
 * 
 * @param image 输入图像
 * @param face 人脸位置
 * @param points 人脸关键点
 * @return bool true表示是真实人脸，false表示是攻击或无法判断
 */
bool seetaface::predict(const SeetaImageData& image, const SeetaRect& face, const SeetaPointF* points) {
    // 使用SeetaFace反欺骗模块预测
    auto status = pFAS->Predict(image, face, points);
    
    // 根据预测状态返回结果
    switch (status) {
        case seeta::FaceAntiSpoofing::REAL:
            std::println("真实人脸"); return true;
        case seeta::FaceAntiSpoofing::SPOOF:
            std::println("攻击人脸"); return false;
        default: // 包括FUZZY和DETECTING状态
            std::println("无法判断或正在检测"); return false;
    }
}

/**
 * @brief 完整的活体检测流程
 * 
 * 该函数执行完整的人脸活体检测流程：
 * 1. 检测人脸位置
 * 2. 标记人脸关键点
 * 3. 进行活体检测
 * 
 * @param image 输入图像
 * @return bool true表示是真实人脸，false表示是攻击或无法判断
 */
bool seetaface::anti_face(const SeetaImageData& image) {
    // 检测人脸位置
    SeetaRect pos = detect(image);
    
    // 检查人脸检测是否成功
    if (pos.width == 0 || pos.height == 0) return false;
    
    // 标记人脸关键点
    auto points = mark(image, pos);
    
    // 进行活体检测
    return predict(image, pos, points.data());
}