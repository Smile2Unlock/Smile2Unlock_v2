# 统一日志验证记录

本记录对应 `ACG-j/unified-logging` 分支完成 review 第 1–5 节后的本地验证（2026-10-22）。工作目录为仓库根目录，Rust 命令除外。未推送，未创建 PR。使用 GCC 16 / MinGW GCC 16、xmake 与 wine-staging 11.20。下列命令的输出重定向到各节列出的 `/tmp` 日志；退出码来自实际运行结果。

## Linux 原生

```sh
xmake f -p linux -a x86_64 -m release --with_slint=n --with_seetaface=n -y
xmake build -r -y su_log_test su_log_sinks_test su_authd pam_smile2unlock su_deploy_helper su_pam_integration_test su_multi_user_auth_test su_theme_test su_session_lock_monitor_smoke_test su_deploy_test
xmake test -v 'su_log_test/*' 'su_log_sinks_test/*' 'su_theme_test/*' 'su_pam_integration_test/*' 'su_multi_user_auth_test/*' 'su_session_lock_monitor_smoke_test/*' 'su_deploy_test/*'
```

三条命令均退出 **0**。完整重建耗时 **22.419 秒**；测试 **7 通过、0 失败**，耗时 **24.197 秒**。构建包含身份验证守护进程、PAM 模块和部署助手；测试覆盖日志核心/sink、主题、PAM、多用户认证、会话锁定监控及部署。PAM 链接使用的 `su_log` 已启用 PIC。

此配置关闭 Slint 与 Seeta，使用 mock 识别后端，不代表真实摄像头认证验收。配置提示未启用后端的 `Ascend SDK not found`，不阻塞构建。

日志：`/tmp/s2u-review-linux-config.log`、`/tmp/s2u-review-linux-build.log`、`/tmp/s2u-review-linux-test.log`。

## MinGW 构建与 Wine

```sh
xmake f -p mingw -a x86_64 -m release --with_slint=y --with_seetaface=y -y
xmake build -r -y su_log_test su_log_sinks_test su_windows_log_security_test su_windows_request_logging_test su_auth_service su_recognition_agent su_deploy_helper su_app su_windows_recognition_result_test su_windows_pipe_response_test su_windows_deploy_helper_result_test
WINEPATH='Z:\usr\x86_64-w64-mingw32\bin' xmake test --output-on-failure -j 1 'su_log_test/*' 'su_log_sinks_test/*' 'su_windows_log_security_test/*' 'su_windows_request_logging_test/*' 'su_windows_recognition_result_test/*' 'su_windows_pipe_response_test/*' 'su_windows_deploy_helper_result_test/*'
```

三条命令均退出 **0**。Slint/Seeta 完整构建耗时 **75.951 秒**；Wine 测试 **7 通过、0 失败**，耗时 **3.217 秒**。配置命令在构建、测试之后再次执行并退出 0，确认配置一致，随后切回 Linux 配置进行上述原生验证。

测试覆盖共享 C++/Rust golden 格式、sink 组合、实际子进程在父进程持有追加句柄时写入、不同 writer 追随轮转后的当前文件、轮转被阻止时保留当前行并在解除阻止后重试。安全测试继续检查不可信所有者/重解析路径拒绝、目标 ACL 不变、SYSTEM-only 产品根目录和 SYSTEM/Administrators 日志目录的访问规则。请求日志测试验证纯操作/状态映射和完成记录字段；既有识别结果、管道响应、部署结果回归均通过。

Wine 11.20 在 `SetSecurityInfo(... PROTECTED_DACL_SECURITY_INFORMATION)` 返回成功后，读回的 SDDL 未包含保护位。测试在 Wine 下**显式跳过保护位断言**，仍断言精确 ACE 规则；真实 Windows 下强制检查保护位。该保护位的原生验收尚未完成。

部署结果测试需要上述 `WINEPATH` 定位既有部署助手使用的 MinGW runtime DLL；新增日志测试采用静态链接。已有 Slint 属性弃用/匿名结构、摄像头 FARPROC 转换及模块警告仍存在。Wine 的 HID/Mesa 等环境诊断不代表测试失败。

日志：`/tmp/s2u-review-mingw-config.log`、`/tmp/s2u-review-mingw-build.log`、`/tmp/s2u-review-wine-test.log`。

## Rust 凭据提供程序

在 `src/platform/windows/credential_provider_rs` 执行：

```sh
cargo test --locked --target x86_64-pc-windows-gnu
cargo clippy --target x86_64-pc-windows-gnu -- -D warnings
cargo fmt --check
```

最终三条命令均退出 **0**。全量单元测试 **119 通过、0 失败、1 项既有忽略**，耗时 **5.04 秒**；另有子进程 writer harness **1 通过**（119 项中包含父测试，不重复累计）。4 个二进制测试 harness 和 doc-tests 均为 0 项并通过。Clippy 无警告，格式检查通过。新增 4 项 Windows logger 测试覆盖子进程写入、共享追加句柄、轮转追随、失败保留和重试。

本轮首次全量测试退出 **101**：**118 通过、1 失败、1 忽略**，失败为既有 `pipe_mode_failure_closes_the_connected_handle`。其旧断言对已关闭的数值句柄调用 `GetHandleInformation`；其他并行测试可能复用该数值。测试现通过仍有效的服务端句柄，在配置失败前确认连接正常，失败后确认 `ERROR_BROKEN_PIPE`，避免依赖失效的句柄数值。只修改测试，未修改管道生产代码、IPC 协议或状态映射。随后上述全量测试通过。

日志：首次失败 `/tmp/s2u-review-rust-test.log`；最终 `/tmp/s2u-review-rust-final-test.log`、`/tmp/s2u-review-rust-clippy.log`。`cargo fmt --check` 无输出、退出 0。

## Review 修订与交付检查

- `24424c4`：恢复认证服务原始文件布局和 `process_request` 签名；在原失败点记录原因，在 worker 的原处理调用处记录完成事件，保留纯映射头。
- `38bd945`：GUI/识别器改用具体事件和字段，移除通用 diagnostic 包装、值中的标签前缀及无参数格式化。
- `2bb9427`：C++/Rust Windows 多进程追加共享、按文件 ID 追随轮转、失败保留当前行、缓存已验证目录句柄及对应测试。
- `bacc5ba`：修正上述既有管道关闭测试的观测方式。
- `31bab1a`：`Record` 使用普通值成员支持移动，并注明部署助手进程 logger 初始化顺序。
- `038b861`：独立的 `fix(windows): reject untrusted owners on protected data directories` 安全提交；提交正文解释普通用户所有者保留隐式 `WRITE_DAC` 的旧风险及新的拒绝行为。`docs/logging.md` 给出管理员修复所有权或删除空目录的步骤。

`local-repo` 已初始化（`7e4b4091cafabf1efacefb42665c9a1f0b477653`）。`git diff --check` 通过。新增 C++ 调用处的 59 个不同字段键均不含脱敏拒绝子串；未削弱拒绝列表。`src/platform/windows/deploy/deployment.cpp` 和 IPC 协议文件相对基线无差异；未改动加密算法。原有超长认证服务文件保留原布局，不为日志任务拆分。

执行：

```sh
git diff --stat dbad70c -- src/platform/windows/auth_service
```

输出：

```text
 .../windows/auth_service/logon_secret_server.cpp   | 159 ++++++++++++++-------
 .../windows/auth_service/logon_secret_server.h     |   4 +-
 src/platform/windows/auth_service/request_log.h    |  81 +++++++++++
 src/platform/windows/auth_service/service_main.cpp |  31 ++--
 4 files changed, 202 insertions(+), 73 deletions(-)
```

尚未进行真实 Windows 11 LTSC、SYSTEM/LogonUI 安全桌面、服务生命周期和实际摄像头端到端验收。GUI 导出诊断按钮按任务范围留待后续。
