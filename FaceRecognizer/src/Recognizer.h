#pragma once
#include <seeta/FaceRecognizer.h>
#include <mutex>
#include <thread>
#include <memory>
#include <seeta/FaceLandmarker.h>
#include <seeta/FaceDetector.h>
#include "Utils.h"
#include <windows.h>


class Recognizer
{
private:
	std::vector<SeetaPointF> points;
	seeta::FaceRecognizer* pFR = new_fr();
	seeta::FaceLandmarker* pFL = new_fl();
	seeta::FaceDetector* pFD = new_fd();
	SeetaImageData cap_image;
	float threshold = 0.7f;

	std::shared_ptr<float> extract(
		seeta::FaceRecognizer* pFD,
		const SeetaImageData& cap_image,
		const std::vector<SeetaPointF>& points);
	float compare(seeta::FaceRecognizer* fr,
		const std::shared_ptr<float>& feat1,
		const std::shared_ptr<float>& feat2);
	seeta::FaceLandmarker* new_fl();
	std::shared_ptr<float> img2features(seeta::FaceDetector* fd, seeta::FaceLandmarker* fl, seeta::FaceRecognizer* fr, SeetaImageData cap_img);
	std::vector<SeetaPointF> mark(seeta::FaceLandmarker* fl, const SeetaImageData& image, const SeetaRect& face);
	seeta::FaceRecognizer* new_fr();
	seeta::FaceDetector* new_fd();
	SeetaRect detect(seeta::FaceDetector* fd, SeetaImageData cap_img);
public:
	Recognizer();
	~Recognizer();
	std::shared_ptr<float> tofeats(SeetaImageData image);
	void savefeat(std::shared_ptr<float> feat, const std::string data_file);
	std::shared_ptr<float> loadfeat(const std::string data_file);
	float feat_compare(std::shared_ptr<float> feat1, std::shared_ptr<float> feat2);
};