#include "CapThread.h"

CapThread::CapThread() {
CapruningThread = std::thread(&CapThread::CapThreadProcess, this);
}

CapThread::~CapThread() {
    keepRunning = false;
    if (CapruningThread.joinable())
    {
		CapruningThread.join();
    }
}

cv::Mat CapThread::getImage()
{
std::lock_guard<std::mutex> lock(cap_img);
    return outputImage;
}

void CapThread::CapThreadProcess() {
    // 打开默认摄像头（0表示系统默认摄像头，多摄像头可尝试1、2等）
    cv::VideoCapture cap(0);

    // 检查摄像头是否成功打开
    if (!cap.isOpened()) {
        std::cerr << "错误：无法打开摄像头！" << std::endl;
    }

    std::cout << "摄像头打开成功！开始读取图像帧（按ESC键退出）..." << std::endl;
    std::cout << "图像尺寸：" << cap.get(cv::CAP_PROP_FRAME_WIDTH) << "x"
        << cap.get(cv::CAP_PROP_FRAME_HEIGHT) << std::endl;

    cv::Mat frame;  // 用于存储每一帧图像
    int frame_count = 0;  // 帧计数器

    // 循环读取摄像头帧
    while (keepRunning) {
        // 读取一帧图像
        cap >> frame;

        // 检查帧是否为空（读取失败）
        if (frame.empty()) {
            std::cerr << "错误：无法获取图像帧！" << std::endl;
            break;
        }

        // 每10帧输出一次状态（避免控制台刷屏）
        if (frame_count % 10 == 0) {
            std::cout << "已读取 " << frame_count << " 帧，当前帧尺寸："
                << frame.cols << "x" << frame.rows << std::endl;
        }
        frame_count++;

        {
            std::lock_guard<std::mutex> lock(cap_img);
			outputImage = frame.clone();
        }
    }
}