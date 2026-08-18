# 剩余安全与可靠性问题

本文记录认证和部署加固后仍需跟踪的问题，与实现计划分开，作为后续评审和发布检查清单使用。

## 高优先级：同账户管理授权

人脸录入、删除和凭据清除原先只检查调用方 SID/UID 以及凭据是否存在。同一账户下的任意进程因此都能修改该账户的认证策略。Windows 相关代码位于 `src/platform/windows/auth_service/logon_secret_server.cpp`，Linux 相关代码位于 `src/modules/su.auth.daemon.cppm`。

管理操作必须使用独立于识别请求的账户授权。建议先验证系统密码/PAM，再签发短时、单次使用的 capability，并绑定 SID/UID、登录会话、操作、随机数和过期时间。识别请求不能签发管理 capability。

状态：部分完成。Windows 在密码验证后签发 256 位、有效期 10 分钟的单次管理令牌，并绑定调用方 SID 和进程；录入和删除会消费并轮换令牌，清除凭据则直接重新验证 Windows 密码。Linux 仍需实现对应的 PAM 管理授权。

## 高优先级：校验提权部署输入

Windows 提权 helper 会从解压目录复制服务、识别 agent 和 CP 二进制到 `Program Files`，随后安装 LocalSystem 服务。如果解压目录可被普通用户修改或在复制时被替换，这条链路会变成高权限代码安装入口。

状态：待修复。应验证带签名 manifest 和 Authenticode，并使用基于句柄、禁止 reparse point 的复制流程；更稳妥的方案是改为正式的 MSI/WiX 安装包。

## 中优先级：限制 Windows 认证管道

命名管道 ACL 允许已认证用户访问，服务端仍以单请求同步方式处理连接。恶意客户端可能持续占用服务。

状态：部分完成。已连接客户端必须在 15 秒内发送完整请求；服务仍未实现按 SID 的连接/请求速率限制。

## 中优先级：锁屏认证使用 GUI 配置

Windows 服务原先硬编码相机索引、活体阈值和识别阈值，CP 也固定发送 Session ID 0，导致用户选择的相机无效，并影响 RDP/多会话。

状态：已完成。GUI 会写入经过范围限制的 per-user 识别设置，服务按 SID 读取并在无效时使用安全默认值，CP 会发送自身真实的 Windows Session ID。

## 中优先级：失败后允许 CP 重试

CP tile 原先在一次 serialization 失败后永久进入 stale 状态，用户无法在当前 tile 重试人脸或手工密码。

状态：已完成。现在只有尚未收到 `ReportResult` 的 serialization 会阻止重复提交；Windows 登录失败后会重置 tile，下一次尝试使用新的 request ID 并重新做人脸认证。

## 中优先级：减少明文密码副本

`CredIsProtectedW` 原先收到不保证 NUL 结尾的切片，部分 `CredProtectW`/`CredPackAuthenticationBufferW` 错误路径也会留下临时密码副本。

状态：已完成已审查路径。CP 在调用 Windows credential API 前显式补 NUL，并在所有已处理的失败路径擦除临时密码、释放内存；C++ 服务传给 `LogonUserW` 的缓冲区也会立即显式清零。

## 中优先级：限制 Linux daemon 并发

Linux daemon 原先为每个连接创建一个无上限的 detached thread。

状态：已完成。socket 读写超时仍为 15 秒，并通过 semaphore 将并发连接 worker 限制为 16 个。

## 工程覆盖

原有的 Rust-only CI 已按要求删除。重新建设 CI 时，需要覆盖 Windows/Linux C++ 构建、打包、服务注册和部署 smoke test。CP crate 的 vendored `memsafe` 仍有格式差异，届时应修正或明确排除。
