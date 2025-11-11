#pragma once
#include <opencv2/opencv.hpp>
#include <thread>
#include <mutex>
#include <vector>
#include <iostream>

class CapThread
{
	private:
		std::mutex cap_img;
		cv::Mat outputImage;
		std::thread CapruningThread;
		std::atomic<bool> keepRunning{ true };
		void CapThreadProcess();

	public:
		CapThread();
		~CapThread();
		cv::Mat getImage();
};

