#include "protocol/messages.h"
#include "seetaface.h"

import std;
import su.facerecognizer.camera;
import su.facerecognizer.pipeline;
import su.facerecognizer.socket_sender;

namespace su::facerecognizer {

namespace {

/**
 * @brief 获取默认连接端点
 * 
 * Windows 平台使用 TCP 127.0.0.1:43101
 * 其他平台使用 Unix Domain Socket /run/smile2unlock/fr.sock
 * 
 * @return std::string 端点地址
 */
std::string default_endpoint() {
#if defined(_WIN32)
    return "127.0.0.1:43101";
#else
    return "/run/smile2unlock/fr.sock";
#endif
}

/**
 * @brief 创建启动消息
 * 
 * 创建一条占位错误消息，用于表示服务启动状态。
 * 
 * @return protocol::OutboundMessage 启动消息
 */
protocol::OutboundMessage make_boot_message() {
    protocol::OutboundMessage message;
    message.type = protocol::MessageType::kError;
    message.error_message = "su_facerecognizer bootstrap placeholder";
    return message;
}

}  // namespace

/**
 * @brief 人脸识别服务主函数
 * 
 * 执行流程：
 * 1. 连接到主应用程序的 Socket 端点
 * 2. 发送启动消息
 * 3. 初始化摄像头、人脸识别引擎和识别流水线
 * 
 * @param args 命令行参数
 * @return int 退出码（0-成功，1-连接失败，2-初始化失败）
 */
int run(std::span<char*> args) {
    (void)args;

    SocketSender sender(default_endpoint());
    if (!sender.connect()) {
        std::cerr << "failed to connect to su_app endpoint\n";
        return 1;
    }

    sender.send(make_boot_message());

    try {
        CameraCapture camera(0);
        SeetaFaceEngine engine;
        RecognitionPipeline pipeline(camera, engine);
        (void)pipeline;
    } catch (const std::exception& exception) {
        std::cerr << "su_facerecognizer init failed: " << exception.what() << '\n';
        return 2;
    }

    return 0;
}

}  // namespace su::facerecognizer

/**
 * @brief 程序入口点
 * 
 * @param argc 参数个数
 * @param argv 参数数组
 * @return int 退出码
 */
int main(int argc, char** argv) {
    return su::facerecognizer::run(std::span(argv, static_cast<std::size_t>(argc)));
}
