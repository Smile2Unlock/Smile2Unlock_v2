# Smile2Unlock Recognizer Library Plan

## Summary

`su_facerecognizer` 不再作为第一阶段默认独立进程。识别能力先重组为 `su_recognizer` 静态库，由 `su_app` 进程内调用。

这样可以减少一条高带宽 socket、减少预览帧跨进程传输、降低调试复杂度。未来如果需要崩溃隔离或权限隔离，再把 `su_recognizer` adapter 后端替换成独立进程。

## Boundary

`su_recognizer` 负责：

- 摄像头枚举。
- 摄像头打开/关闭。
- 预览帧捕获。
- libyuv 像素格式转换。
- SeetaFace6 人脸检测。
- 关键点定位。
- 特征提取。
- 活体检测。
- 特征向量比对。

`su_recognizer` 不负责：

- UI 渲染。
- 用户数据库。
- 认证策略。
- 配置持久化。
- PAM / Credential Provider。
- control socket。

## Language Use

- C++: 识别模块主语言，负责 SeetaFace、libyuv、摄像头 API 集成。
- Rust: 不直接调用 SeetaFace，只通过 C++ AppController 获取识别结果。
- Zig: 可为路径、权限、平台 helper 提供 C ABI 工具，但不进入识别核心。
- ASM: 只作为可选 SIMD 优化，必须有 scalar fallback。

## Public API Shape

第一阶段使用 C++ typed API。

示意：

```cpp
struct CameraInfo {
    int index;
    std::string name;
};

struct PreviewFrame {
    int width;
    int height;
    std::vector<std::byte> rgba_or_rgb;
};

struct FaceBox {
    int x;
    int y;
    int width;
    int height;
};

struct RecognitionResult {
    bool has_face;
    std::optional<FaceBox> face_box;
    std::vector<float> feature;
    float liveness_score;
};

class RecognizerService {
public:
    std::vector<CameraInfo> enumerate_cameras();
    std::expected<void, RecognizerError> open_camera(int camera_index);
    std::expected<PreviewFrame, RecognizerError> capture_preview_frame();
    std::expected<RecognitionResult, RecognizerError> extract_features();
    std::expected<float, RecognizerError> compare_features(
        std::span<const float> lhs,
        std::span<const float> rhs);
    void close_camera();
};
```

具体接口可以调整，但必须遵守：

- 不暴露 SeetaFace 原始对象。
- 不暴露平台摄像头句柄。
- 不通过字符串拼接表达错误。
- 使用 typed result。
- 热路径 buffer 允许复用，但复用策略封装在实现内部。

## Functional Style

识别模块中，纯计算和副作用要分离：

- 摄像头读取是副作用。
- 像素转换可以封装成纯转换接口。
- 特征比对是纯函数。
- 阈值判断是纯函数。
- UI 状态更新不进入识别模块。

允许受控 mutation：

- 摄像头 buffer。
- libyuv 输入输出 buffer。
- SeetaFace 模型对象生命周期。

这些 mutation 必须封装在 RAII 类型内部。

## Future Process Split

如果未来恢复 `su_facerecognizer` 独立进程，规则如下：

- `su_app` 仍调用 `RecognizerService` 抽象。
- 进程内实现和 socket 实现都实现同一 adapter。
- UI 和 Rust core 不感知识别模块是在进程内还是进程外。
- 预览帧协议只能隐藏在 adapter 内部。

未来可选结构：

```text
su_app
  -> RecognizerService interface
  -> InProcessRecognizerService

or

su_app
  -> RecognizerService interface
  -> SocketRecognizerService
  -> su_facerecognizer process
```

## Test Plan

- camera mock 测试。
- feature compare 纯函数测试。
- libyuv wrapper 测试。
- SeetaFace wrapper smoke test。
- 无摄像头环境下的 mock pipeline 测试。
- 可选 SIMD 与 scalar fallback 一致性测试。
