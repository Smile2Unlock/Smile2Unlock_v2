# Smile2Unlock C++ Modules Plan

## 与重写主计划的关系

本模块方案**不与 `docs/rewrite_master_plan.md` 冲突**，理由：

- **正交的编译期优化**：模块化只改变 C++ 代码的组织和编译方式，不影响运行时架构（su_app / su_core / su_recognizer / PAM 的 target 结构和调用链不变）
- **目录兼容**：`src/modules/` 是新增目录，不改变已有的 `src/app/`、`src/recognizer/`、`src/core-rs/` 结构
- **Phase 无关**：可以在任何 Phase（0-5）中并行推进，不依赖 Phase 完成顺序
- **不影响非 C++ target**：Rust core、Zig、PAM 不受影响

## Rationale

当前 `src/` 中的 C++ 代码使用传统头文件（`.h`），存在以下问题：

- `recognizer_service.h` 定义的共享类型（ImageView、FaceBox、RecognitionResult 等）被 5+ 个 TU 包含
- 每个 `.cpp` 文件单独解析头文件，增大了增量编译时间
- 宏定义（`SU_HAS_SEETAFACE`、`SU_HAS_SLINT`）通过头文件扩散

C++20 Modules 解决这些问题：模块只编译一次、明确的导出边界、不再依赖头文件顺序。

## Scope

**只覆盖 `src/` 中的代码**。以下内容**不**做模块化：

- `common/`、`Smile2Unlock/`、`FaceRecognizer/` — 旧架构遗留代码
- 第三方 C/C++ 头文件（SeetaFace、Slint、libyuv、V4L2）
- Rust FFI C ABI 头（`su_core.h`）— 纯 C 导出，由 `import <cstddef>` 等替代
- `.slint` UI 文件 — 由 Slint 编译器生成 `app_window.h`，保持 `#include`
- 测试文件（`tests/`）— 可以 `import` 模块，但不做模块化改造

## 模块层次

```
su.recognizer.types        ← 底层：共享类型（零依赖其他模块）
su.core.types               ← 底层：core FFi 类型（零依赖其他模块）
  ├── su.recognizer.backend ← 中间：SeetaFace 封装（依赖 types）
  ├── su.recognizer.camera  ← 中间：V4L2 封装（依赖 types）
  ├── su.recognizer.image   ← 中间：像素转换（依赖 types）
  ├── su.recognizer.service ← 上层：识别服务（依赖 backend + camera + image）
  ├── su.core.bridge        ← 上层：FFI 桥接（依赖 core.types + Rust C ABI）
  └── su.app.controller     ← 顶层：AppController（依赖 service + bridge）
      └── su.app.preview    ← 顶层：预览控制器（依赖 service + Slint）
```

### 模块文件结构

```
src/
├── modules/                        ← C++ Module 接口单元 (.cppm)
│   ├── su.recognizer.types.cppm
│   ├── su.core.types.cppm
│   ├── su.recognizer.backend.cppm
│   ├── su.recognizer.camera.cppm
│   ├── su.recognizer.image.cppm
│   ├── su.recognizer.service.cppm
│   ├── su.core.bridge.cppm
│   ├── su.app.controller.cppm
│   └── su.app.preview.cppm
├── recognizer/                     ← 实现文件 (常规 .cpp，import 模块)
│   ├── recognizer_service.cpp
│   ├── seetaface_backend.cpp
│   ├── camera/v4l2_camera.cpp
│   ├── image/pixel_convert.cpp
│   └── image/image_loader.cpp
├── app/                            ← 实现文件
│   ├── app_controller.cpp
│   ├── core_bridge.cpp
│   ├── preview_controller.cpp
│   ├── slint_main.cpp
│   └── console_main.cpp
└── core-rs/include/su_core.h       ← 保持 #include（纯 C 导出）
```

## 详细模块定义

### su.recognizer.types

```cpp
// src/modules/su.recognizer.types.cppm
export module su.recognizer.types;

import std;

export namespace su::recognizer {

export enum class RecognizerError {
    kNoCamera, kCameraUnavailable, kModelUnavailable,
    kInvalidArgument, kInvalidImage, kImageLoadFailed, kNoFace,
};

export struct CameraInfo { int index = 0; std::string name; };
export struct PreviewFrame { int width = 0; int height = 0; std::vector<std::byte> rgba_or_rgb; };
export struct FaceBox { int x = 0; int y = 0; int width = 0; int height = 0; };
export struct RecognitionResult { bool has_face = false; std::optional<FaceBox> face_box; std::vector<float> feature; float liveness_score = 0.0F; };
export struct ImageView { int width = 0; int height = 0; int channels = 0; std::span<const std::byte> bytes; };
export struct CapturedFrame { int width = 0; int height = 0; std::uint32_t v4l2_format = 0; std::span<const std::byte> bytes; };

export std::string embedding_sample_source(std::span<const float> feature);

} // namespace su::recognizer
```

### su.core.types

```cpp
// src/modules/su.core.types.cppm
export module su.core.types;

import std;

export namespace su::app {

export enum class CoreError {
    kNullArgument, kInvalidUtf8, kUserDenied, kIoError,
    kParseError, kWriteError, kInvalidArgument, kBufferTooSmall, kUnknown,
};

export struct CoreConfig {
    std::uint32_t version = 1;
    int selected_camera = 0;
    float recognition_threshold = 0.65F;
    bool liveness_detection = true;
    float liveness_threshold = 0.50F;
    std::uint32_t preview_fps = 15;
};

export struct AuthDecision { bool accepted = false; };
export struct FaceAuthDecision { bool accepted = false; float score = 0.0F; std::uint32_t profile_count = 0; };
export struct FaceProfileSummary { std::string id; std::string label; std::uint64_t created_at_unix = 0; };
export struct FaceAuthReport { /* ... full struct ... */ };

export std::uint32_t core_version_major();
export std::expected<float, CoreError> default_threshold();
export CoreConfig default_config();
export std::expected<CoreConfig, CoreError> load_config(const std::string& path);
// ... 所有 FFI 桥接函数 ...

} // namespace su::app
```

### su.recognizer.backend

```cpp
// src/modules/su.recognizer.backend.cppm
export module su.recognizer.backend;

import std;
import su.recognizer.types;

export namespace su::recognizer {

export struct SeetaFaceModelPaths { /* ... */ };
export std::filesystem::path default_seetaface_model_dir();
export std::expected<SeetaFaceModelPaths, RecognizerError> seetaface_model_paths(const std::filesystem::path&);

export class SeetaFaceBackend {
public:
    explicit SeetaFaceBackend(SeetaFaceModelPaths);
    ~SeetaFaceBackend();
    SeetaFaceBackend(SeetaFaceBackend&&) noexcept;
    SeetaFaceBackend& operator=(SeetaFaceBackend&&) noexcept;

    std::expected<RecognitionResult, RecognizerError> extract(ImageView, bool liveness_enabled) const;
    std::expected<RecognitionResult, RecognizerError> predict_liveness(ImageView, bool liveness_enabled) const;
    bool available() const;
    bool liveness_available() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace su::recognizer
```

### su.recognizer.service

```cpp
// src/modules/su.recognizer.service.cppm
export module su.recognizer.service;

import std;
import su.recognizer.types;
import su.recognizer.backend;
import su.recognizer.camera;
import su.recognizer.image;

export namespace su::recognizer {

export class RecognizerService {
public:
    RecognizerService();
    ~RecognizerService();
    RecognizerService(RecognizerService&&) noexcept;
    RecognizerService& operator=(RecognizerService&&) noexcept;

    std::vector<CameraInfo> enumerate_cameras() const;
    std::expected<void, RecognizerError> open_camera(int camera_index);
    std::expected<PreviewFrame, RecognizerError> capture_preview_frame() const;
    std::expected<RecognitionResult, RecognizerError> extract_features() const;
    std::expected<float, RecognizerError> compare_features(std::span<const float>, std::span<const float>) const;
    void close_camera();
    std::expected<std::pair<PreviewFrame, RecognitionResult>, RecognizerError> capture_and_extract() const;
    std::expected<RecognitionResult, RecognizerError> extract_from_image(ImageView, bool liveness_enabled = true) const;
    std::expected<RecognitionResult, RecognizerError> predict_liveness(ImageView, bool liveness_enabled = true) const;
    bool seetaface_available() const;

private:
    std::expected<void, RecognizerError> ensure_seetaface_backend() const;
    std::optional<int> active_camera_;
    // ... private members ...
};

} // namespace su::recognizer
```

### su.app.controller

```cpp
// src/modules/su.app.controller.cppm
export module su.app.controller;

import std;
import su.core.types;
import su.recognizer.types;
import su.recognizer.service;

export namespace su::app {

export struct AppSnapshot { /* ... */ };
export struct FaceDemoSnapshot { /* ... */ };

export class AppController { /* ... */ };

} // namespace su::app
```

### su.app.preview

```cpp
// src/modules/su.app.preview.cppm
export module su.app.preview;

import std;
import su.recognizer.types;
import su.recognizer.service;

// Slint 仍是传统 #include（通过 Slint 编译器生成的 C++ 头）
#include <slint.h>

export namespace su::app {

export struct PreviewOverlay { /* ... */ };

#if SU_HAS_SLINT
export class PreviewController { /* ... */ };
#endif

} // namespace su::app
```

## xmake.lua 配置

### 1. 启用 Modules 支持（在文件顶部）

```lua
add_rules("mode.debug", "mode.release")
set_policy("build.cxxmodules", true)       -- ← 新增
```

### 2. 注册模块文件

模块接口单元（`.cppm`）在各自 target 中注册。由于 Modules 需要正确的编译顺序，**所有模块应集中在同一个 target 或在顶层全局注册**。

推荐：在顶层 `add_files` 中注册所有模块接口单元：

```lua
-- 全局注册（在所有 target 之前，确保 BMI 生成顺序）
add_files("src/modules/*.cppm")
```

### 3. 更新 target 依赖

`su_recognizer` target 不再需要 `add_headerfiles()` 中的模块类型头：

```lua
target("su_recognizer")
    apply_cpp_target("static")
    add_files("src/recognizer/*.cpp")
    add_files("src/recognizer/image/*.cpp")
    add_files("src/recognizer/camera/*.cpp")
    -- add_headerfiles("src/recognizer/*.h")        ← 去掉
    -- add_headerfiles("src/recognizer/image/*.h")  ← 去掉
    -- add_headerfiles("src/recognizer/camera/*.h") ← 去掉
    add_packages("cimg")
    add_packages("libyuv")
    -- ...
```

`su_app` target 同理。

### 4. 条件编译宏

`SU_HAS_SEETAFACE`、`SU_HAS_SLINT` 等宏仍然通过 `add_defines` 传递到模块内部。

在模块接口单元中，用条件 `import` 或 `#if` 保护对可选后端的依赖：

```cpp
module;
#include <seeta/FaceDetector.h>  // 第三方头用 include（module; 前缀使其私有）
export module su.recognizer.backend;
// ...
```

## 迁移步骤

### Phase 1: 类型模块（低风险）

1. 在 `xmake.lua` 加 `set_policy("build.cxxmodules", true)`
2. 创建 `src/modules/su.recognizer.types.cppm`
3. 创建 `src/modules/su.core.types.cppm`
4. 从 `recognizer_service.h` 和 `core_bridge.h` 复制类型定义到模块文件
5. 保留 `.h` 文件（兼容阶段）
6. `xmake build` 验证

### Phase 2: 中间模块

7. 创建 `su.recognizer.backend.cppm` / `.camera.cppm` / `.image.cppm`
8. 将 `seetaface_backend.h`、`v4l2_camera.h` 等中的声明移入模块
9. 保留 `.h` 文件（兼容阶段）
10. `xmake build` 验证

### Phase 3: 上层模块 + 删除头文件

11. 创建 `su.recognizer.service.cppm` / `su.core.bridge.cppm`
12. 切换 `.cpp` 实现文件从 `#include "header.h"` → `import module.name;`
13. 创建 `su.app.controller.cppm` / `su.app.preview.cppm`
14. 删除 `src/recognizer/*.h`、`src/app/*.h` 等已模块化的头文件
15. 删除 `add_headerfiles()` 中对应的条目
16. `xmake build + test` 全量验证

## 风险与注意事项

| 风险 | 缓解措施 |
|------|----------|
| **GCC 模块支持仍不完善** | 保留 `.h` 头文件备份；先做 Phase 1（类型模块，纯声明，风险最低） |
| **`import std;` 一次性标准库** | 推荐使用 `import std;`（C++23）替代逐个 `import <header>`；GCC 14+ 支持。若编译报错可退回 `import <header>;` 细粒度导入 |
| **条件编译宏与模块作用域** | `#define` 在模块内不是 export 的；用 `add_defines` 或 `module;` 私有区 |
| **`std::unique_ptr<Impl>` 前向声明** | 模块内仍需要前向声明；`import` 不影响 PIMPL 惯用法 |
| **Slint 生成的头文件** | 不能模块化；在 `su.app.preview` 中用 `#include "app_window.h"`（`module;` 私有区） |
| **增量编译性能退化** | Modules 的 BMI 缓存可能导致首次编译变慢；后续增量编译显著加快 |
| **多 target 共享 BMI** | xmake 自动管理 `build/.gens/*/rules/bmi/cache/`；确保模块在顶层 `add_files` |

## 模块与头文件对照表

| 现有头文件 | 对应的模块 | 迁移优先级 |
|-----------|-----------|-----------|
| `recognizer/recognizer_service.h`（类型部分） | `su.recognizer.types` | P0 |
| `recognizer/recognizer_service.h`（class 部分） | `su.recognizer.service` | P2 |
| `recognizer/seetaface_backend.h` | `su.recognizer.backend` | P1 |
| `recognizer/camera/v4l2_camera.h` | `su.recognizer.camera` | P1 |
| `recognizer/image/pixel_convert.h` | `su.recognizer.image` | P1 |
| `recognizer/image/image_loader.h` | `su.recognizer.image` | P1 |
| `app/core_bridge.h`（类型部分） | `su.core.types` | P0 |
| `app/core_bridge.h`（函数部分） | `su.core.bridge` | P2 |
| `app/app_controller.h` | `su.app.controller` | P2 |
| `app/preview_controller.h` | `su.app.preview` | P2 |
