#pragma once
#include <seeta/FaceRecognizer.h>
#include <seeta/FaceLandmarker.h>
#include <seeta/FaceDetector.h>
#include "Utils.h"
#include <seeta/FaceAntiSpoofing.h>
#include <iostream>


class Recognizer
{
private:
	std::vector<SeetaPointF> points;
	seeta::FaceRecognizer* pFR = nullptr;
	seeta::FaceLandmarker* pFL = nullptr;
	seeta::FaceDetector* pFD = nullptr;
	seeta::FaceAntiSpoofing* pFAS = nullptr;
	SeetaImageData cap_image;
	float threshold = 0.7f;
	
	// 保存模型路径字符串,防止悬空指针
	std::string model_path_fd;
	std::string model_path_fl;
	std::string model_path_fr;
	std::string model_path_fas1;
	std::string model_path_fas2;

	std::shared_ptr<float> extract(
		seeta::FaceRecognizer* pFD,
		const SeetaImageData& cap_image,
		const std::vector<SeetaPointF>& points);
	float compare(seeta::FaceRecognizer* fr,
		const std::shared_ptr<float>& feat1,
		const std::shared_ptr<float>& feat2);
	std::shared_ptr<float> img2features(seeta::FaceDetector* fd, seeta::FaceLandmarker* fl, seeta::FaceRecognizer* fr, SeetaImageData cap_img);
	std::vector<SeetaPointF> mark(seeta::FaceLandmarker* fl, const SeetaImageData& image, const SeetaRect& face);
	SeetaRect detect(seeta::FaceDetector* fd, SeetaImageData cap_img);
	bool predict(const SeetaImageData& image, const SeetaRect& face, const SeetaPointF* points);

public:
	Recognizer();
	~Recognizer();
	std::shared_ptr<float> tofeats(SeetaImageData image);
	void savefeat(std::shared_ptr<float> feat, const std::string data_file);
	std::shared_ptr<float> loadfeat(const std::string data_file);
	float feat_compare(std::shared_ptr<float> feat1, std::shared_ptr<float> feat2);
	void reset_video();
	bool anti_face(const SeetaImageData& image);
};