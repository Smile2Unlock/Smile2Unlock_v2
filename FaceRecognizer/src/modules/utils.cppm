//
// Created by ation_ciger on 2026/3/19.
//
export module utils;

import std;

export class Utils {
public:
    static void SaveFeaturesBinary(std::shared_ptr<float> features, int size, const std::string& data_file);
    static std::shared_ptr<float> LoadFeaturesBinary(const std::string& data_file, int size);
    static std::string getCurrentDirectory();
};