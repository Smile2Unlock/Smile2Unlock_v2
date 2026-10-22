# 日志与故障排查

所有组件采用同一行格式（UTF-8、logfmt 字段），例如：

```text
2026-10-22T14:39:02.123+08:00 INFO auth-service 1234:5678 event=request.completed request_id=42 op=authenticate sid=S-1-5-18 status=unavailable duration_ms=135 win32=38
```

固定部分依次为带毫秒和 UTC 偏移的时间、级别、组件、进程 ID 与线程 ID；后面是 `event` 和上下文字段。WARN/ERROR 的 C++ 记录附加调用处文件名与行号，不包含完整源码路径。凭据提供程序保留 COM 诊断消息于 `detail` 字段，频繁的字段查询和描述符调用只在 DEBUG 级别输出。

## 保存位置

| 组件 | 位置/输出 |
| --- | --- |
| Windows 身份验证服务 | `%ProgramData%\Smile2Unlock\Logs\auth-service.log` |
| Windows 识别代理 | 同目录 `recognition-agent.log` |
| Windows 凭据提供程序（Rust） | 同目录 `credential-provider.log` |
| Windows 提权部署助手 | 同目录 `deploy-helper.log` |
| Windows GUI | `%LOCALAPPDATA%\Smile2Unlock\Logs\app.log`，同时输出 stderr/调试器 |
| Linux GUI | `$XDG_STATE_HOME/smile2unlock/logs/app.log`；未设置时使用 `~/.local/state/smile2unlock/logs/app.log`，同时输出 stderr |
| Linux 身份验证守护进程与部署助手 | stderr，由 systemd/journald 保存 |
| Linux PAM 模块 | `pam_syslog`，`LOG_AUTHPRIV` |

文件达到 1 MiB 前尝试轮转，依次保留 `.1`、`.2`、`.3` 三个历史文件；单条记录不跨文件切割。每个进程的同一文件 sink 共享互斥锁和延迟打开的句柄。Windows C++ 与 Rust 文件句柄允许其他进程读、写、删除共享，并保留追加权限；轮转失败时仍通过已校验的句柄追加本条记录，下次再尝试。达到轮转阈值时比较当前路径与句柄的文件 ID，发现其他进程已经轮转则重新打开；多进程下大小上限近似。目录链只在打开/重新打开或轮转时验证，并保留目录句柄，不逐行重复检查。凭据提供程序使用自己的 Rust 实现，不链接 C++。

Windows 系统日志目录仅由身份验证服务启动、部署助手安装/修复（`--register-cp`、`--ensure-service`）创建。识别代理和凭据提供程序从不创建目录。目录不存在或安全校验失败时，仅向 `OutputDebugStringW` 回退；文件日志失败不影响认证结果。

`Smile2Unlock` 产品根目录保持 SYSTEM-only 的受保护 DACL。日志子目录的受保护 DACL 仅允许 SYSTEM 与 Administrators 完全控制。预先存在、所有者不可信或为重解析点的目录会被拒绝，不会接管或修复。校验路径各级目录时保留不允许删除共享的句柄；文件以追加权限和 `FILE_FLAG_OPEN_REPARSE_POINT` 打开，再检查句柄属性，拒绝重解析点。轮转前同样检查现存文件。不要通过放宽 Users 权限解决日志访问问题。

注册凭据提供程序时，部署助手会尽力删除旧 `%WINDIR%\Temp\su_cp.log`。新版本不再写入该文件。

## 级别与配置

默认级别为 `info`，可选 `debug`、`info`、`warn`、`error`，大小写不敏感。Windows 系统组件与凭据提供程序在初始化时读取 64 位注册表：

```powershell
reg add HKLM\SOFTWARE\Smile2Unlock /v LogLevel /t REG_SZ /d debug /f
```

所有组件也支持环境变量 `SMILE2UNLOCK_LOG_LEVEL`，优先于注册表。设置只读取一次，修改后重启对应组件；LogonUI 内 DLL 需重新加载才能使用新值。无效设置回到 `info`，并输出一条 `log.level_invalid` 警告。

```sh
SMILE2UNLOCK_LOG_LEVEL=debug su_app
```

## 数据约束

不要在调用处记录密码、管理令牌、nonce、人脸特征/嵌入、存储密钥。SID 可以记录，避免记录用户名。字段键名包含以下任一子串（不区分大小写）时，格式化器防御性替换值为 `[redacted]`：

```text
password passwd secret token nonce feature embedding key credential
```

这不是内容扫描器；禁止把敏感数据放进 `detail` 等通用字段。包含空格、等号、引号、反斜杠、控制字符或空值的值使用双引号；反斜杠、引号、换行、回车、制表符和其他 ASCII 控制字节分别转义。无效 UTF-8 字节替换为 U+FFFD。单个值最多 512 字节（含 `…[truncated]` 标记，不截断 UTF-8 字符）；字段键/组件名规范为字母、数字、点、短横线和下划线，避免日志注入。

## 收集日志

优先在 GUI「诊断」页点击「导出诊断包」，完成后点击「打开所在位置」。内容与提权说明见下方「一键导出诊断包」。需要手动排查时可使用以下方式。

Windows 使用管理员终端复制上述系统日志及 `.1` 至 `.3`，并收集当前用户的 `app.log*`。若目录被拒绝，先检查所有者、ACL 和重解析属性，并通过调试输出查看 `log.directory_rejected`。不要删除或改写不可信路径指向的目标，也不要让普通用户拥有系统日志目录。

受保护的密码/档案数据目录也拒绝不可信所有者。这是单独的安全修复：旧代码仅重设 DACL，原普通用户所有者仍拥有隐式 `WRITE_DAC`；现在这类安装会拒绝启动相关存储，而不会自动接管目录。管理员修复时应先停止身份验证服务、核对被拒绝的具体路径并排除重解析点。对确认是真实目录的路径，可在管理员终端逐个执行 `takeown /F "<被拒绝的目录>" /A`，将所有者设为 Administrators，再启动服务，由安全创建逻辑重设所需 DACL；不要递归穿越链接。若确认只是预建的空目录，也可删除该目录后让安装器/服务重新创建。删除真实数据目录会丢失已保存凭据和人脸档案，应先妥善备份，并准备重新绑定/录入。

Linux 使用：

```sh
journalctl -u su-authd.service --since '30 minutes ago'
journalctl -u su-deploy-helper.service --since '30 minutes ago'
journalctl --since '30 minutes ago' | rg 'pam_smile2unlock|event='
```

实际 PAM syslog 的落盘位置由发行版决定，也可能在 `/var/log/auth.log` 或 `/var/log/secure`。发送前检查日志中的 SID、路径和设备信息是否需要人工脱敏。

身份验证服务在原有处理调用处记录 `request.completed`（包括无效请求与容量拒绝），携带 `op`、`status`、`duration_ms`、`request_id`、`sid`。底层 Win32 错误、识别代理状态/退出码、存储或档案错误在原失败点记录，按请求 ID 或进程/线程及时间关联；日志不改变 IPC 协议及状态映射。原有认证服务文件保持原始布局，仅做局部日志修改。

## 开发接口

`src/common/log/core.*` 仅包含值与纯变换，不读时钟、不做 I/O、不持有全局状态。`Record` 使用普通值成员以支持移动，格式化器通过 `const Record&` 只读消费。`Logger` 保存不可变配置，`with({...})` 返回绑定请求上下文的新值。sink 是返回 `std::expected<void, SinkError>` 的函数；`fallback` 只在失败后调用后备 sink，`tee` 始终调用两个 sink。Logger 捕获日志内部异常并回退到 stderr/Windows 调试输出，失败不传播到业务分支。

`log_error<T>(logger, "event")` 可用于 `std::expected<T, E>::or_else`；领域类型通过 ADL 的 `to_fields(error)` 定义诊断字段并原样传播错误。`su_log` 是静态 xmake 目标，PAM 保持静态 C++ runtime 与 `--exclude-libs,ALL`。

C++ 与 Rust 同时消费 `tests/log/format_golden.txt`，其中每行以 `|` 分隔输入记录与预期输出，字节使用 C 风格转义。这些用例锁定级别、时区、转义、UTF-8 修复、截断与脱敏格式。Windows 安全测试验证不可信所有者和重解析路径被拒绝且目标 ACL 不变，并检查目录访问规则。Wine 11.20 未报告 ACL 更新后的保护位，因此该位的断言仅在真实 Windows 执行；Wine 下显式输出跳过提示。Wine 测试不等同于真实 Windows/安全桌面验收。完整命令和已知限制见 [验证记录](logging-verification.md)。

## 一键导出诊断包

在 GUI 的「诊断」页点击「导出诊断包」。导出在后台进行，不影响其他页面操作。
Windows 会通过部署助手请求 UAC 提权，Linux 会通过部署助手的 D-Bus 方法请求
polkit 管理员认证。完成后状态栏显示文件路径，可点击「打开所在位置」。

诊断包保存在当前用户桌面；Linux 按 XDG 桌面目录、`~/Desktop`、`$HOME` 依次回退。
文件名为 `smile2unlock-diagnostics-YYYYMMDD-HHMMSS.zip`（Windows）或 `.tar.gz`（Linux），
同名时追加 `-1`、`-2`，不会覆盖已有文件。包内包含：

- `summary.txt`：产品及 Rust 核心版本、系统版本、带 UTC 偏移的本地时间、服务与存储保护状态、
  识别器、摄像头名称及数量、档案数量、桌面/PAM 目标状态和路径、模型状态与目录。
  摘要不写用户名或人脸档案标签，也不复制配置、档案或密码。
- `app/`：GUI 使用的日志目录中的 `app.log` 及数字后缀轮转日志，每个文件最多保留末尾 8 MiB。
- `system/`：Windows 系统组件日志、指定注册表配置、服务/凭据提供程序状态、安装文件大小、
  SHA-256 与 Authenticode 状态、最近七天相关崩溃和 Code Integrity 事件（每类最多 20 条）。
  旧 `su_cp.log` 只记录是否存在及大小，不复制内容。Linux 包含认证服务、部署助手及 PAM 的
  journal、服务状态和 `/etc/pam.d/` 中包含 Smile2Unlock 的行；文本总量最多 4 MiB。

拒绝提权或助手不可用时仍会生成诊断包，`system/unavailable.txt` 说明原因，GUI 标注
「未包含系统日志」。Windows 提权助手只在受保护的 `%ProgramData%\Smile2Unlock-Diagnostics` 目录中暂存数据，
GUI 负责在自己的临时目录打包；完成后尽力删除两处暂存目录，助手下次运行也会清理旧目录。
助手只在收集系统日志期间启用 `SeBackupPrivilege`，结束后恢复原状态，不修改产品数据目录或日志目录的 DACL。
日志及系统事件可能包含本机路径和应用运行信息，发送给开发者前可先检查包内文本。
