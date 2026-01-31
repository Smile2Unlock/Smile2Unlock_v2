#pragma once

#include <string>
#include <vector>
#include <memory>
#include <filesystem>

class Utils {
public:
    static void SaveFeaturesBinary(std::shared_ptr<float> features, int size, const std::string& data_file);
    static std::shared_ptr<float> LoadFeaturesBinary(const std::string& data_file, int size);
    static std::string getCurrentDirectory();
};