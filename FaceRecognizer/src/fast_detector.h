#pragma once

#include <opencv2/opencv.hpp>
#include <seeta/FaceDetector.h>
#include <filesystem>
#include <memory>
#include <iostream>
#include <stdexcept>
#include <direct.h>
#include <string>

// 轻量级人脸检测器 - 只加载检测模型，不加载识别和活体检测
class FastDetector {
public:
    FastDetector() {
        std::cout << "[FastDetector] 快速初始化中（仅检测模型）..." << std::endl;

        // 获取当前目录（简化版本，避免依赖 Utils）
        char currentDir[_MAX_PATH];
        if (_getcwd(currentDir, sizeof(currentDir)) == nullptr) {
            throw std::runtime_error("获取当前目录失败");
        }

        std::string model_path_fd = std::string(currentDir) + "\\resources\\models\\face_detector.csta";

        if (!std::filesystem::exists(model_path_fd)) {
            throw std::runtime_error("人脸检测模型文件不存在: " + model_path_fd);
        }

        seeta::ModelSetting setting_fd;
        setting_fd.append(model_path_fd.c_str());
        pFD = new seeta::FaceDetector(setting_fd);

        std::cout << "[FastDetector] 快速初始化完成" << std::endl;
    }

    ~FastDetector() {
        if (pFD) {
            delete pFD;
            pFD = nullptr;
        }
    }

    // 仅检测人脸，不提取特征
    SeetaRect detect(const SeetaImageData& cap_img) {
        try {
            auto faces = pFD->detect(cap_img);
            if (faces.size > 0) {
                return faces.data[0].pos; // 返回第一张人脸的位置
            }
        } catch (const std::exception& e) {
            std::cerr << "[FastDetector] 检测错误: " << e.what() << std::endl;
        }
        return SeetaRect{0, 0, 0, 0}; // 未检测到人脸
    }

private:
    seeta::FaceDetector* pFD = nullptr;
};
