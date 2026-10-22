# 统一日志验证记录

本记录对应 `ACG-j/unified-logging` 分支的本地验证（2026-10-22）。工作目录为仓库根目录，Rust 命令除外。未推送，未创建 PR。使用 GCC 16 / MinGW GCC 16、xmake 与 wine-staging 11.20。

## Linux 原生

```sh
xmake f -p linux -a x86_64 -m release --with_slint=n --with_seetaface=n -y
xmake build -r -y su_log_test su_log_sinks_test su_authd pam_smile2unlock su_deploy_helper su_pam_integration_test su_multi_user_auth_test su_theme_test su_session_lock_monitor_smoke_test su_deploy_test
xmake build -y su_log_test su_log_sinks_test su_authd pam_smile2unlock su_deploy_helper su_pam_integration_test su_multi_user_auth_test su_theme_test su_session_lock_monitor_smoke_test su_deploy_test
xmake test -v 'su_log_test/*' 'su_log_sinks_test/*' 'su_theme_test/*' 'su_pam_integration_test/*' 'su_multi_user_auth_test/*' 'su_session_lock_monitor_smoke_test/*' 'su_deploy_test/*'
```

以上命令最终均退出 0。完整重建耗时 22.867 秒；最后针对 PAM 配置读取调整的增量构建耗时 1.526 秒。最终测试 **7 通过、0 失败**，耗时 24.139 秒。此配置关闭 Slint 与 Seeta，使用 mock 识别后端；不代表真实摄像头认证验收。

完整重建曾暴露静态 `su_log` 缺少 PIC，导致 PAM 链接失败；已添加 `-fPIC` 后重新完整构建并通过。配置提示非本次启用后端的 `Ascend SDK not found`，不阻塞构建。

最终日志：`/tmp/s2u-linux-config.log`、`/tmp/s2u-linux-build.log`、`/tmp/s2u-linux-final-build.log`、`/tmp/s2u-linux-final-test.log`。

## MinGW 构建与 Wine

```sh
xmake f -p mingw -a x86_64 -m release --with_slint=y --with_seetaface=y -y
xmake build -r -y su_log_test su_log_sinks_test su_windows_log_security_test su_windows_request_logging_test su_auth_service su_recognition_agent su_deploy_helper su_app su_windows_recognition_result_test su_windows_pipe_response_test su_windows_deploy_helper_result_test
xmake build -y su_windows_log_security_test
WINEPATH='Z:\usr\x86_64-w64-mingw32\bin' xmake test --output-on-failure -j 1 'su_log_test/*' 'su_log_sinks_test/*' 'su_windows_log_security_test/*' 'su_windows_request_logging_test/*' 'su_windows_recognition_result_test/*' 'su_windows_pipe_response_test/*' 'su_windows_deploy_helper_result_test/*'
```

真实 Slint/Seeta 配置的最终完整构建退出 0，耗时 70.057 秒。随后增量重建了安全测试，退出 0。最终测试退出 0，**7 通过、0 失败**，耗时 3.248 秒；其中保护位断言在 Wine 下显式跳过，详见下文。测试覆盖纯格式、sink、拒绝不可信所有者/重解析路径、目标 ACL 不变、SYSTEM-only 产品根目录与 SYSTEM/Administrators 日志目录访问规则、请求完成日志唯一性及异常展开，以及既有识别结果、管道响应和部署结果回归。

新增请求测试首次构建缺少 `su_core.h` 的 include 目录，已修复并完整重建。ACL 正向测试的前置目录显式指定 Administrators 所有者；检查 SYSTEM-only DACL 时使用事先打开的安全句柄，避免要求普通 Wine 测试账户在权限收紧后重新打开目录。初版前置条件曾导致测试失败，已修正；未放宽生产代码的所有者或 ACL 检查。

Wine 11.20 在 `SetSecurityInfo(... PROTECTED_DACL_SECURITY_INFORMATION)` 返回成功后，读回的 SDDL 未包含保护位。因此测试在 Wine 下**显式跳过保护位断言**，继续断言根目录只有 SYSTEM、Logs 只有 SYSTEM 与 Administrators 的完整访问规则；真实 Windows 下仍强制检查保护位。该保护位的原生验收尚未完成。

部署结果测试需要上述 `WINEPATH` 以定位既有部署助手使用的 MinGW runtime DLL；新增日志测试采用静态链接。已有 Slint 属性弃用/匿名结构、摄像头 FARPROC 转换及模块警告仍存在。Wine 输出包含 HID/Mesa 等环境诊断；它不等同于真实 Windows 的 ACL、LogonUI 安全桌面、服务生命周期或摄像头验收。

最终日志：`/tmp/s2u-mingw-final-config.log`、`/tmp/s2u-mingw-final-build.log`、`/tmp/s2u-mingw-security-build.log`、`/tmp/s2u-mingw-final-test.log`。

## Rust 凭据提供程序

在 `src/platform/windows/credential_provider_rs` 执行：

```sh
cargo test --locked --target x86_64-pc-windows-gnu
cargo clippy --target x86_64-pc-windows-gnu -- -D warnings
cargo fmt --check
```

最终均退出 0。单元测试 **115 通过、0 失败、1 项既有忽略**；4 个二进制测试 harness 与 doc-tests 均为 0 项并通过。Clippy 无警告；格式检查通过。C++/Rust 均读取同一份 `tests/log/format_golden.txt`。

较早完整测试曾在 `pipe_mode_failure_closes_the_connected_handle` 的 `GetHandleInformation(client).unwrap_err()` 处失败；随后两次完整测试通过。尚未确认该瞬态失败的根因，没有为此修改 IPC 协议或状态映射。本次另做了当前工具链要求的等价 Clippy 清理（原子更新 API、直接返回 match、合并条件、固定宽度分块）。

最终日志：`/tmp/s2u-rust-test.log`、`/tmp/s2u-rust-clippy.log`。

## 边界与交付检查

`local-repo` 已通过 `git submodule update --init local-repo` 初始化。`git diff --check` 通过。`src/platform/windows/deploy/deployment.cpp`、IPC 协议与状态映射保持原有行为；未改动加密算法。新增/拆出的日志和服务实现文件均小于 800 行，避免继续扩展原有超大服务文件。

尚未进行真实 Windows 11 LTSC、SYSTEM/安全桌面和实际摄像头端到端验收。GUI 导出诊断按钮按任务范围留待后续。
