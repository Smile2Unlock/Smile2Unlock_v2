#pragma once

#include <fstream>

class Utils
{
public:
	Utils() = default;
	~Utils() = default;

	// 获取可执行文件所在目录的绝对路径
	static std::string GetExecutableDir();

	// 获取项目根目录（从可执行文件向上查找包含 resources 文件夹的目录）
	static std::string GetProjectRoot();

	// 将特征以自定义二进制格式保存到文件（带魔数、version、count、data、CRC32）
	// 返回 true 表示写入成功
	static bool SaveFeaturesBinary(const std::shared_ptr<float>& features, int size, const std::string& filepath);

	// 从文件读取特征并执行 CRC 校验，out_size 返回维度，失败返回 nullptr
	static std::shared_ptr<float> LoadFeaturesBinary(const std::string& filepath, int& out_size);
};