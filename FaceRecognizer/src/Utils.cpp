#include "Utils.h"
#include <fstream>    // 使用C++文件流
#include <stdexcept>  // 添加缺失的头文件
#include <iostream>

/**
 * @brief 将人脸特征以二进制格式保存到文件
 * 
 * @param features 人脸特征向量
 * @param size 特征向量大小
 * @param data_file 保存文件路径（仅文件名部分）
 */
void Utils::SaveFeaturesBinary(std::shared_ptr<float> features, int size, const std::string& data_file) {
    if (!features) {
        throw std::invalid_argument("无效的特征向量指针");
    }
    if (size <= 0) {
        throw std::invalid_argument("特征向量大小必须大于0");
    }
    if (data_file.empty()) {
        throw std::invalid_argument("文件名不能为空");
    }
    
    // 获取当前目录并构建完整文件路径
    std::string current_dir = getCurrentDirectory();
    if (current_dir.empty()) {
        throw std::runtime_error("无法获取当前目录");
    }
    
    std::filesystem::path save_path = std::filesystem::path(current_dir) / "data" / "face" / data_file;
    
    // 确保目录存在
    try {
        std::filesystem::create_directories(save_path.parent_path());
    } catch (const std::filesystem::filesystem_error& e) {
        throw std::runtime_error("无法创建目录: " + std::string(e.what()));
    }
    
    // 使用C++文件流，自动管理资源
    std::ofstream file(save_path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("无法打开文件进行写入: " + save_path.string());
    }
    
    // 写入数据并检查是否成功
    file.write(reinterpret_cast<const char*>(features.get()), sizeof(float) * size);
    if (!file) {
        throw std::runtime_error("写入文件失败: " + save_path.string());
    }
}

/**
 * @brief 从二进制文件加载人脸特征
 * 
 * @param data_file 特征文件路径（仅文件名部分）
 * @param size 特征向量大小
 * @return std::shared_ptr<float> 加载的人脸特征向量
 */
std::shared_ptr<float> Utils::LoadFeaturesBinary(const std::string& data_file, int size) {
    if (size <= 0) {
        throw std::invalid_argument("特征向量大小必须大于0");
    }
    if (data_file.empty()) {
        throw std::invalid_argument("文件名不能为空");
    }
    
    // 获取当前目录并构建完整文件路径
    std::string current_dir = getCurrentDirectory();
    if (current_dir.empty()) {
        throw std::runtime_error("无法获取当前目录");
    }
    
    std::filesystem::path load_path = std::filesystem::path(current_dir) / "data" / "face" / data_file;
    
    // 使用C++文件流，自动管理资源
    std::ifstream file(load_path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("无法打开文件进行读取: " + load_path.string());
    }
    
    // 创建特征向量
    std::shared_ptr<float> features(
        new float[size], 
        [](float* p) { delete[] p; }  // 自定义删除器，使用delete[]释放数组
    );
    
    // 读取数据并检查是否成功
    file.read(reinterpret_cast<char*>(features.get()), sizeof(float) * size);
    if (!file) {
        throw std::runtime_error("读取文件失败: " + load_path.string());
    }
    
    return features;
}

/**
 * @brief 获取当前工作目录
 * 
 * @return std::string 当前工作目录路径
 */
std::string Utils::getCurrentDirectory() {
    try {
        std::cout << "当前工作目录: " << std::filesystem::current_path() << std::endl;
        return std::filesystem::current_path().string();
    } catch (const std::filesystem::filesystem_error& e) {
        throw std::runtime_error("获取当前目录失败: " + std::string(e.what()));
    }
}