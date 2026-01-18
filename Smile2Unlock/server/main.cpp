#include "../ipc/IPCSender.h"
#include "../../FaceRecognizer/src/Camera.h"
#include <queue>
#include <memory>

// 全局帧队列
std::queue<cv::Mat> frames;

int main() {
    // 初始化状态码
    status_code = 0;
    
    // 创建IPC发送器和摄像头实例
    auto ipc = std::make_unique<IPCSender>();
    auto cam = std::make_unique<Camera>();
    
    // 检查摄像头是否成功打开
    if (!cam->isOpened()) {
        std::cerr << "Error: Could not open camera." << std::endl;
        return -1;
    }
    
    std::cout << "Smile2Unlock Server started successfully." << std::endl;
    std::cout << "Camera opened, IPC sender running..." << std::endl;
    
    // 运行30秒进行测试
    std::this_thread::sleep_for(std::chrono::milliseconds(30000));
    
    std::cout << "Server shutting down..." << std::endl;
    return 0;
}
