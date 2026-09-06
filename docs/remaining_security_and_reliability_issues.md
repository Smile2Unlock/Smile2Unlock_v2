# 剩余安全与可靠性问题

本文记录认证和部署加固后仍需跟踪的问题，与实现计划分开，作为后续评审和发布检查清单使用。最近复核：2026-09-05；完整状态入口见 `current_status.md`。

## 高优先级：同账户管理授权

人脸录入、删除和凭据清除原先只检查调用方 SID/UID 以及凭据是否存在。同一账户下的任意进程因此都能修改该账户的认证策略。Windows 相关代码位于 `src/platform/windows/auth_service/logon_secret_server.cpp`，Linux 相关代码位于 `src/modules/su.auth.daemon.cppm`。

管理操作必须使用独立于识别请求的账户授权。建议先验证系统密码/PAM，再签发短时、单次使用的 capability，并绑定 SID/UID、登录会话、操作、随机数和过期时间。识别请求不能签发管理 capability。

状态：已完成。Windows 在密码验证后签发 256 位、有效期 10 分钟的单次管理令牌，并绑定调用方 SID 和进程。Linux 通过 Polkit 的活动本地会话授权签发 256 位、有效期 2 分钟的单次 capability，并绑定 UID、socket peer PID、登录会话和精确操作；应用协议不传递明文系统密码。

## 高优先级：校验提权部署输入

Windows 提权 helper 会从解压目录复制服务、识别 agent 和 CP 二进制到 `Program Files`，随后安装 LocalSystem 服务。如果解压目录可被普通用户修改或在复制时被替换，这条链路会变成高权限代码安装入口。

状态：代码完成，发布证据待补。部署只接受带 detached CMS 签名的 `release-info.json`，验证每个 PE 的 Authenticode 与清单 SHA-256；源文件和目标目录拒绝 reparse point，文件通过保持句柄的临时写入、flush 和原子替换安装。正式发布仍需用受信任的发布证书生成 ZIP，并在真实 Windows 信任链下验证。

## 中优先级：限制 Windows 认证管道

命名管道 ACL 允许已认证用户访问，服务端仍以单请求同步方式处理连接。恶意客户端可能持续占用服务。

状态：已完成。已连接客户端必须在 15 秒内发送完整请求；服务在读取请求前执行每 SID 连接令牌桶，并在完整请求后执行请求令牌桶。桶数量有上限，空闲 SID 会清理；隔离和补充行为有纯单元测试及 Wine 回归测试。

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

仓库已恢复 GitHub Actions 工作流，覆盖 Linux C++ 全构建与 Xmake 测试、Rust core、Linux tar 包、Windows 特权组件交叉构建、MinGW/Wine Credential Provider 和按 SID 限流测试，以及使用临时 CI code-signing 身份生成并验证 Windows 签名 ZIP。临时证书只验证流水线闭环，不能替代正式发布证书。

2026-09-06 复核：工作流三个 job 已用 act + Docker 在本地干净环境全部跑绿（`scripts/local-ci.sh`），并修复了六个只在干净环境暴露的构建/配置问题；托管 runner 仍需实际运行一次确认。

## 发布完整性

- 主题 monitor 已在损坏 DMS/Matugen 输入时保留最后一个有效主题，并报告被拒绝的来源；回归测试通过。
- 正式发布前仍必须由同一提交重新构建并验证 Windows ZIP，确认正式证书信任链、Authenticode、CMS 和清单 SHA-256；不得复用工作区中的旧 ZIP。
- `cimg` 已固定为 `v4.0.4`。
- SeetaFace6Open 本地包配方已固定到验收构建使用的 `a32e2faa0694c0f841ace4df9ead0407b78363c6`；该提交的 gitlinks 固定递归子模块版本，仍需在托管 runner 的干净缓存中验证一次。
- 两个 Rust crate 的 `Cargo.lock` 已纳入版本控制，Xmake 和 CI 均使用 `--locked`；stable channel 实际解析 rustc 1.98.1 (48a229cea 2026-09-01)，已记录于 2026-09-06 本地验证。
- Rust FFI 已增加显式 out-parameter API，C++ 调用方先初始化结构体并有 ABI 尺寸断言；GCC `-Wmaybe-uninitialized` 已消除。
- Linux PAM 升级/回滚已使用写前日志、备份指纹、降级拒绝、启动恢复和幂等批量回滚；真实发行版包管理器与断电场景仍需 VM 验收。
