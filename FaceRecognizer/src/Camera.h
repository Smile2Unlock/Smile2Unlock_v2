#pragma once
#include <opencv2/opencv.hpp>
#include <queue>
#include <thread>
#include <chrono>
#include <vector>

extern std::queue<cv::Mat> frames;

class Camera {
private:
    cv::VideoCapture cap;
    std::vector<std::jthread> threads;

public:
    Camera(int index = 0) {
        cap.open(index);
        // 使用lambda函数正确包装成员函数调用
        threads.emplace_back([this, id = 0](std::stop_token stoken) {
            this->writeFrame(stoken, id);
        });
    }

    ~Camera() {
        threads.clear();
        cap.release();
    }

    bool isOpened() {
        return cap.isOpened();
    }

    bool readFrame(cv::Mat& frame) {
        return cap.read(frame);
    }

    bool writeFrame(std::stop_token stoken, int id) {
        cv::Mat frame;
        while (!stoken.stop_requested()) {
            if (!cap.read(frame)) {
                return false;
            }
            frames.push(frame);
            std::this_thread::sleep_for(std::chrono::milliseconds(30)); // 控制帧率
        }
        return true;
    }
};
