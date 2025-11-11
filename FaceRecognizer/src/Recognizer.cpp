#pragma optimize("", off)
#include "Recognizer.h"


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

}

Recognizer::~Recognizer()
{

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

seeta::FaceLandmarker* Recognizer::new_fl()
{
	seeta::ModelSetting setting;
	std::string project_root = Utils::GetProjectRoot();
	std::string model_path = project_root + "\\resources\\models\\face_landmarker_pts5.csta";
	
	setting.append(model_path.c_str());
	return new seeta::FaceLandmarker(setting);
}

std::vector<SeetaPointF> Recognizer::mark(seeta::FaceLandmarker* fl, const SeetaImageData& image, const SeetaRect& face)
{
	std::vector<SeetaPointF> points = fl->mark(image, face);
	return points;
}

seeta::FaceRecognizer* Recognizer::new_fr()
{
	seeta::ModelSetting setting;
	std::string project_root = Utils::GetProjectRoot();
	std::string model_path = project_root + "\\resources\\models\\face_recognizer.csta";
	
	const char* models[] = { model_path.c_str(), NULL };
	setting.model = models;
	return new seeta::FaceRecognizer(setting);
}

seeta::FaceDetector* Recognizer::new_fd()
{
	seeta::ModelSetting setting;
	std::string project_root = Utils::GetProjectRoot();
	std::string model_path = project_root + "\\resources\\models\\face_detector.csta";
	
	setting.append(model_path.c_str());
	return new seeta::FaceDetector(setting);
}

SeetaRect Recognizer::detect(seeta::FaceDetector* fd, SeetaImageData cap_img)
{
	try
	{
		SeetaFaceInfoArray faces = fd->detect(cap_img);
		if (faces.size == 0) {
			throw std::runtime_error("No face detected\n");
		}
		return faces.data[0].pos;
	}
	catch (const std::exception& e)
	{
		printf("SeetaRect{ 0, 0, 0, 0 }\n");
		// 返回一个无效的人脸框表示检测失败
		return SeetaRect{ 0, 0, 0, 0 };
	}
	catch (...)
	{
		printf("SeetaRect{ 0, 0, 0, 0 }\n");
		// 捕获所有其他未知异常
		return SeetaRect{ 0, 0, 0, 0 };
	}
}
