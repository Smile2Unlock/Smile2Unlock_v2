#pragma once
#ifdef _WIN32
#ifdef FRLIBRARY_EXPORTS
#define FRLIBRARY_API __declspec(dllexport)
#else
#define FRLIBRARY_API __declspec(dllimport)
#endif
#else
#define FRLIBRARY_API __attribute__((visibility("default")))
#endif

#include "src/Recognizer.h"
#include "src/CapThread.h"
#include <string>

class FRLIBRARY_API Dllloader
{
public:
	Dllloader();
	~Dllloader();

	// 初始化摄像头
	bool InitializeCamera();
	
	// 获取摄像头帧
	bool GetCameraFrame();
	
	// 人脸录入功能
	bool FaceEnrollment(const std::string& dataFile);
	
	// 人脸识别功能
	bool FaceRecognition(const std::string& dataFile);
	
	// 获取识别结果
	float GetSimilarity();
	
	// 获取识别状态
	int GetRecognitionStatus();
	
	// 清理资源
	void Cleanup();

private:
	CapThread* capThread;
	Recognizer* recognizer;
	cv::Mat currentFrame;
	float currentSimilarity;
	int recognitionStatus; // 0:未开始, 1:识别中, 2:识别成功, 3:识别失败, 4:活体检测失败
	bool cameraInitialized;
};

// C风格导出函数，便于外部调用
extern "C" {
	FRLIBRARY_API bool FR_InitializeCamera();
	FRLIBRARY_API bool FR_FaceEnrollment(const char* dataFile);
	FRLIBRARY_API bool FR_FaceRecognition(const char* dataFile);
	FRLIBRARY_API float FR_GetSimilarity();
	FRLIBRARY_API int FR_GetRecognitionStatus();
	FRLIBRARY_API void FR_Cleanup();
}