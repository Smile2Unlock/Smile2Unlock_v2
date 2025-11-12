#pragma optimize("", off)
#include <opencv2/opencv.hpp>  
#include "src/CapThread.h" // 将尖括号替换为双引号以包含本地头文件
#include <seeta/FaceDatabase.h>
#include "src/Recognizer.h"
import std;


namespace seeta
{
    namespace cv
    {
        // using namespace ::cv;
        class ImageData : public SeetaImageData {
        public:
            ImageData(const ::cv::Mat& mat)
                : cv_mat(mat.clone()) {
                this->width = cv_mat.cols;
                this->height = cv_mat.rows;
                this->channels = cv_mat.channels();
                this->data = cv_mat.data;
            }
        private:
            ::cv::Mat cv_mat;
        };
    }
}

int main() {

    int mode;
    std::cout << "请选择模式（1-人脸录入，2-人脸识别）：";
    std::cin >> mode;

    CapThread* capThread = new CapThread();
    // 等待摄像头线程填充首帧（最多等待 3s）
    cv::Mat frame;
    const int max_wait_ms = 5000;
    int waited = 0;
    while (waited < max_wait_ms) {
        frame = capThread->getImage();
        if (!frame.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        waited += 50;
    }

    if (frame.empty()) {
        std::cerr << "错误：未能获取首帧，退出。" << std::endl;
        delete capThread; // 析构函数会停止线程并 join
        return -1;
    }

	// 进行人脸录入
    if (mode == 1)
    {
        Recognizer* recognizer = new Recognizer();
        SeetaImageData image_data_frame = seeta::cv::ImageData(frame);
        auto feat = recognizer->tofeats(image_data_frame);
        recognizer->savefeat(feat, "face_data.bin");
        std::cout << "人脸录入成功" << std::endl;
    }
    if (mode == 2)
    {
        Recognizer* recognizer = new Recognizer();
        std::cout << "正在进行人脸识别，请稍候..." << std::endl;
		auto feat1 = recognizer->loadfeat("face_data.bin");
		std::shared_ptr<float> feat2;
        int time_count = 0;
        int anti_count = 0;
        while (true)
        {
            //TODO:活体检测
			frame = capThread->getImage();
            SeetaImageData image_data_frame = seeta::cv::ImageData(frame);
			auto anti = recognizer->anti_face(image_data_frame);
            if (!anti)
            {
                anti_count++;
            }
            if (anti_count > 50)
            {
	            break;
            }

            //人脸识别
			feat2 = recognizer->tofeats(image_data_frame);
			auto similar = recognizer->feat_compare(feat1, feat2);
            if (similar > 0.62f and anti)
            {
                std::cout << "识别成功，相似度：" << similar << std::endl;
                break;
            }
            std::cout << "识别中，相似度：" << similar << std::endl;
            if (similar < 0.62f and time_count > 10)
            {
                std::cout << "识别失败，相似度：" << similar << std::endl;
                break;
			}
            time_count++;
        }

	}

    return 0;
}