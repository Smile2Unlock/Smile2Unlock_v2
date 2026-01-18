#pragma optimize("", off)
#include "Recognizer.h"
import std;


std::shared_ptr<float> Recognizer::img2features(seeta::FaceDetector* fd, seeta::FaceLandmarker* fl,
                                                seeta::FaceRecognizer* fr, SeetaImageData cap_img)
{
	SeetaRect pos = Recognizer::detect(pFD, cap_img);
	points = Recognizer::mark(pFL, cap_img, pos);
	return  Recognizer::extract(pFR, cap_img, points);
}

void Recognizer::savefeat(std::shared_ptr<float> feat, const std::string data_file)
{
	int size = pFR->GetExtractFeatureSize();
	Utils::SaveFeaturesBinary(feat, size, data_file);
}

std::shared_ptr<float> Recognizer::loadfeat(const std::string data_file)
{
	int size = pFR->GetExtractFeatureSize();
	auto feat = Utils::LoadFeaturesBinary(data_file, size);
	return feat;
}

float Recognizer::feat_compare(std::shared_ptr<float> feat1, std::shared_ptr<float> feat2)
{
	return Recognizer::compare(pFR, feat1, feat2);
}

Recognizer::Recognizer()
{
	std::cout << "[Recognizer] 初始化中..." << std::endl;
	
	// 初始化模型路径字符串 (避免悬空指针)
	std::string project_root = Utils::GetProjectRoot();
	model_path_fd = project_root + "\\resources\\models\\face_detector.csta";
	model_path_fl = project_root + "\\resources\\models\\face_landmarker_pts5.csta";
	model_path_fr = project_root + "\\resources\\models\\face_recognizer.csta";
	model_path_fas1 = project_root + "\\resources\\models\\fas_first.csta";
	model_path_fas2 = project_root + "\\resources\\models\\fas_second.csta";
	
	try {
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
		
		// 活体检测暂时禁用
		pFAS = nullptr;
		
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

Recognizer::~Recognizer()
{
	if (pFD) delete pFD;
	if (pFL) delete pFL;
	if (pFR) delete pFR;
	if (pFAS) delete pFAS;
}

std::shared_ptr<float> Recognizer::tofeats(SeetaImageData image)
{
	return Recognizer::img2features(pFD, pFL, pFR,image);
}

std::shared_ptr<float> Recognizer::extract(
	seeta::FaceRecognizer* fr,
	const SeetaImageData& image,
	const std::vector<SeetaPointF>& points) {
	std::shared_ptr<float> features(
		new float[fr->GetExtractFeatureSize()],
		std::default_delete<float[]>());
	fr->Extract(image, points.data(), features.get());
	return features;
}

float Recognizer::compare(seeta::FaceRecognizer* fr,
	const std::shared_ptr<float>& feat1,
	const std::shared_ptr<float>& feat2) {
	return fr->CalculateSimilarity(feat1.get(), feat2.get());
}

std::vector<SeetaPointF> Recognizer::mark(seeta::FaceLandmarker* fl, const SeetaImageData& image, const SeetaRect& face)
{
	std::vector<SeetaPointF> points = fl->mark(image, face);
	return points;
}

SeetaRect Recognizer::detect(seeta::FaceDetector* fd, SeetaImageData cap_img)
{
	SeetaFaceInfoArray faces = fd->detect(cap_img);
	if (faces.size == 0) {
		throw std::runtime_error("No face detected");
	}
	return faces.data[0].pos;
}

bool Recognizer::predict(const SeetaImageData& image, const SeetaRect& face,
	const SeetaPointF* points)
{
	if (!pFAS) return true; // 活体检测禁用时默认通过
	
	auto status = pFAS->Predict(image, face, points);
	switch (status) {
	case seeta::FaceAntiSpoofing::REAL:
		std::cout << "真实人脸" << std::endl;
		return true;
	case seeta::FaceAntiSpoofing::SPOOF:
		std::cout << "攻击人脸" << std::endl;
		return false;
	case seeta::FaceAntiSpoofing::FUZZY:
		std::cout << "无法判断" << std::endl;
		return false;
	case seeta::FaceAntiSpoofing::DETECTING:
		std::cout << "正在检测" << std::endl;
		return false;
	}
	return false;
}

void Recognizer::reset_video()
{
	if (pFAS) pFAS->ResetVideo();
}

bool Recognizer::anti_face(const SeetaImageData& image)
{
	SeetaRect pos = Recognizer::detect(pFD, image);
	points = Recognizer::mark(pFL, image, pos);
	return Recognizer::predict(image, pos, points.data());
}
