#include <opencv2/opencv.hpp>
#include <queue>
#include <thread>  // 添加缺少的头文件
#include <chrono>  // 添加缺少的头文件

extern std::queue<cv::Mat> frames;

class camera {
    private:
        cv::VideoCapture cap;
        std::vector<std::jthread> threads;
    public:
        camera(int index = 0) {
            cap.open(index);
            // 修复：使用lambda函数正确包装成员函数调用
            threads.emplace_back([this, id = 0](std::stop_token stoken) {
                this->writeFrame(stoken, id);
            });
        }

        ~camera() {
            threads.clear();
            cap.release();
            cv::destroyAllWindows();
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