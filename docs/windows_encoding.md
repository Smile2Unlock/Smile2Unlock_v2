# Windows 编码约定与验收

应用文本、Slint 文本、Rust C FFI 路径和模型库路径使用 UTF-8。Windows 系统路径在读取和访问期间保留 UTF-16，仅在进入文本接口时明确转为 UTF-8。凭据和密码继续使用 Windows 原生 UTF-16，保持原字符序列。

## 边界约定

- 通过 `su::windows::environment_path` 读取路径环境变量；禁止将窄 `getenv()` 返回值当作 UTF-8 路径。
- TEMP 通过 `temporary_directory` 的 `GetTempPathW` 读取，GUI 日志和模型下载文件直接用原生路径访问。
- APPDATA/USERPROFILE 共用 `roaming_app_data`，配置 TOML 与 UI JSON 落在同一账户目录。环境变量缺失时使用 Windows 已知文件夹，不把配置悄悄重定向到当前目录或 TEMP。
- 程序位置通过动态长度的 `GetModuleFileNameW` 获取，禁止用 Windows 窄 `argv[0]` 定位语言包、部署 helper 和模型。模型查找优先级是显式环境覆盖、可执行文件所在目录向上查找、构建目录/当前目录回退。
- `std::filesystem::path` 保持原生路径类型。用 `su::path_utf8` 导出文本，用 `su::path_from_utf8` 导入 UTF-8 文本；不要依赖不同标准库中窄 `path`/`string()` 的隐式编码约定。
- SeetaFace 的路径参数按 UTF-8 导出；本地依赖包在 ORZ、SeetaAuthorize 的加密 `.csta` 读取、TenniS（含加密和内联流）及 SDK 模型读取入口执行严格 UTF-8 → UTF-16 转换，再使用宽字符文件流/`_wfopen`。无效 UTF-8 被拒绝，不回退到系统 ANSI 页。
- TenniS 用 `GetModuleFileNameW` 定位自身 DLL，并用 `LoadLibraryW` 加载 CPU 变体。MinGW 构建使用其实际产物的 `libtennis*.dll` 名称，MSVC 保留 `tennis*.dll` 名称。
- 依赖包配置 `unicode_paths=true` 纳入缓存标识，避免继续链接修复前的 DLL。依赖源码补丁对固定上游版本检查替换数量，版本漂移会导致构建失败。
- 配置和语言包文件内容约定为 UTF-8。支持不同系统代码页，不等于自动读取 GBK/UTF-16 配置。

## 自动回归

`su_windows_unicode_paths_test` 在 Windows/Wine 中测试 ASCII、中文与空格、日文、重音字母及补充平面字符目录；覆盖原生环境变量、USERPROFILE 回退、Unicode TEMP、长环境值、Rust 配置保存与读回、UI JSON 原子保存与读回、损坏 JSON 的 UTF-8 路径诊断，以及从 Unicode 目录、无关工作目录启动子进程后准确获取程序路径。

`su_windows_unicode_models_test` 将五个正式模型复制到同时含上述 Unicode 字符的目录，通过原生模型环境变量读取，初始化检测、关键点、识别和活体模型；再读取 Unicode 文件名的 PNG，完成真实人脸特征提取及活体调用。测试程序和 SDK DLL 也移动到 Unicode 目录，并显式触发 CPU 切换、检查已加载 DLL 的原生位置；较新 CPU 不一定自动进入旧 CPU 所需的切换分支。这能检测应用与第三方 DLL 的组合，而非只测试路径转换函数。

用户名与部署结果文件继续使用 `su_windows_username_utf8_test`、`su_username_initial_test` 和 `su_windows_deploy_helper_result_test` 验证。

## 真机验收

在 Windows 中文 936、日文 932、西欧 1252 和 UTF-8 65001 代码页下，覆盖不同语言用户名及安装目录。验证 GUI 启动、语言包读取、配置保存和重启恢复、模型下载、识别、活体检测，以及提权 helper 的结果文件读取。日志应记录实际 `GetACP()`，不能把字节模拟或 Wine 一种代码页测试记作四套真机通过。

重新发布时必须重建受影响的 EXE/DLL，重新签名、生成发布清单和清单签名，再构建安装包；旧安装包不会因为源码修复而自动兼容。
