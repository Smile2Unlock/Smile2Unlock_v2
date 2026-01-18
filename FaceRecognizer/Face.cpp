#pragma optimize("", off)
#include <opencv2/opencv.hpp>  
#include "src/CapThread.h" // 将尖括号替换为双引号以包含本地头文件
#include <seeta/FaceDatabase.h>
#include "src/Recognizer.h"
#include "src/Utils.h"
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

    std::cout << "=== Smile2Unlock FaceRecognizer Debug ===" << std::endl;
    
    // 检查路径
    std::string exe_dir = Utils::GetExecutableDir();
    std::string project_root = Utils::GetProjectRoot();
    std::cout << "可执行文件目录: " << exe_dir << std::endl;
    std::cout << "项目根目录: " << project_root << std::endl;
    std::cout << "模型目录: " << project_root << "\\resources\\models" << std::endl;
    
    int mode;
    std::cout << "\n请选择模式（1-人脸录入，2-人脸识别）：";
    std::cin >> mode;

    std::cout << "初始化摄像头..." << std::endl;
    CapThread* capThread = nullptr;
    
    try {
        capThread = new CapThread();
    } catch (const std::exception& e) {
        std::cerr << "摄像头初始化失败: " << e.what() << std::endl;
        return -1;
    }
    
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
    
    std::cout << "摄像头初始化成功，首帧尺寸: " << frame.cols << "x" << frame.rows << std::endl;

	// 进行人脸录入
    if (mode == 1)
    {
        std::cout << "\n=== 人脸录入模式 ===" << std::endl;
        std::cout << "正在加载识别模型..." << std::endl;
        
        Recognizer* recognizer = nullptr;
        try {
            recognizer = new Recognizer();
        } catch (const std::exception& e) {
            std::cerr << "模型加载失败: " << e.what() << std::endl;
            delete capThread;
            return -1;
        }
        
        std::cout << "请面向摄像头，按下 'Space' 键进行录入，按 'ESC' 退出" << std::endl;
        
        cv::namedWindow("人脸录入", cv::WINDOW_NORMAL);
        
        bool captured = false;
        while (!captured) {
            frame = capThread->getImage();
            if (frame.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                continue;
            }
            
            cv::Mat display = frame.clone();
            cv::putText(display, "Press SPACE to enroll", cv::Point(10, 30), 
                       cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(0, 255, 0), 2);
            cv::imshow("人脸录入", display);
            
            int key = cv::waitKey(30);
            if (key == 27) { // ESC
                std::cout << "用户取消录入" << std::endl;
                delete recognizer;
                delete capThread;
                cv::destroyAllWindows();
                return 0;
            }
            else if (key == 32) { // Space
                captured = true;
            }
        }
        
        std::cout << "正在提取人脸特征..." << std::endl;
        seeta::cv::ImageData image_data_wrapper(frame);
        SeetaImageData& image_data_frame = image_data_wrapper;
        
        auto feat = recognizer->tofeats(image_data_frame);
        
        if (feat == nullptr) {
            std::cerr << "错误：未检测到人脸！" << std::endl;
            std::cerr << "请确保：1) 脸部在画面中心 2) 光线充足 3) 正面面向摄像头" << std::endl;
            delete recognizer;
            delete capThread;
            cv::destroyAllWindows();
            return -1;
        }
        
        recognizer->savefeat(feat, "face_data.bin");
        std::cout << "✓ 人脸录入成功！特征已保存到 face_data.bin" << std::endl;
        
        delete recognizer;
        cv::destroyAllWindows();
    }
    if (mode == 2)
    {
        std::cout << "\n=== 人脸识别模式 ===" << std::endl;
        std::cout << "正在加载识别模型..." << std::endl;
        
        Recognizer* recognizer = new Recognizer();
        
        auto feat1 = recognizer->loadfeat("face_data.bin");
        if (feat1 == nullptr) {
            std::cerr << "错误：未找到 face_data.bin，请先进行人脸录入（模式1）！" << std::endl;
            delete recognizer;
            delete capThread;
            return -1;
        }
        
        std::cout << "请面向摄像头进行识别，按 'ESC' 退出" << std::endl;
        
        cv::namedWindow("人脸识别", cv::WINDOW_NORMAL);
        
        int time_count = 0;
        int anti_count = 0;
        
        while (true)
        {
            frame = capThread->getImage();
            if (frame.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                continue;
            }
            
            cv::Mat display = frame.clone();
            seeta::cv::ImageData image_data_wrapper(frame);
            SeetaImageData& image_data_frame = image_data_wrapper;
            
            // 活体检测
            auto anti = recognizer->anti_face(image_data_frame);
            if (!anti)
            {
                anti_count++;
                cv::putText(display, "Liveness FAILED", cv::Point(10, 30), 
                           cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(0, 0, 255), 2);
            }
            else {
                cv::putText(display, "Liveness OK", cv::Point(10, 30), 
                           cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(0, 255, 0), 2);
            }
            
            if (anti_count > 50)
            {
                cv::putText(display, "TOO MANY LIVENESS FAILURES", cv::Point(10, 70), 
                           cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(0, 0, 255), 2);
                cv::imshow("人脸识别", display);
                cv::waitKey(2000);
                break;
            }

            // 人脸识别
            auto feat2 = recognizer->tofeats(image_data_frame);
            if (feat2 == nullptr) {
                cv::putText(display, "No face detected", cv::Point(10, 110), 
                           cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(0, 165, 255), 2);
            }
            else {
                auto similar = recognizer->feat_compare(feat1, feat2);
                
                char sim_text[100];
                sprintf(sim_text, "Similarity: %.3f", similar);
                cv::putText(display, sim_text, cv::Point(10, 110), 
                           cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(255, 255, 0), 2);
                
                if (similar > 0.62f && anti)
                {
                    cv::putText(display, "AUTHENTICATED!", cv::Point(10, 150), 
                               cv::FONT_HERSHEY_SIMPLEX, 1.5, cv::Scalar(0, 255, 0), 3);
                    cv::imshow("人脸识别", display);
                    cv::waitKey(2000);
                    std::cout << "✓ 识别成功！相似度：" << similar << std::endl;
                    break;
                }
                
                if (similar < 0.62f && time_count > 30)
                {
                    cv::putText(display, "FAILED - Low similarity", cv::Point(10, 150), 
                               cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(0, 0, 255), 2);
                    cv::imshow("人脸识别", display);
                    cv::waitKey(2000);
                    std::cout << "✗ 识别失败，相似度过低：" << similar << std::endl;
                    break;
                }
            }
            
            cv::imshow("人脸识别", display);
            int key = cv::waitKey(30);
            if (key == 27) { // ESC
                std::cout << "用户取消识别" << std::endl;
                break;
            }
            
            time_count++;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        delete recognizer;
        cv::destroyAllWindows();
    }

    delete capThread;
    return 0;
}