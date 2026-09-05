# Smile2Unlock 当前状态与剩余工作

更新日期：2026-09-05。本文是当前实现状态的入口；其他 `*_plan.md` 保留设计背景和历史记录，其中未勾选项不一定代表当前代码尚未实现。

## 已实现主线

- Linux：Slint GUI、V4L2/SeetaFace 识别、system-owned 加密档案、`su_authd`、PAM bridge、control socket、D-Bus/Polkit 部署 helper，以及 DMS、KScreenLocker、Plasma Login、GDM 和 SDDM 的受控 PAM 转换。
- Windows：纯 Rust Credential Provider、LocalSystem 认证服务、加密的 per-SID 人脸档案与账户凭据、目标会话识别 agent、命名管道协议、UAC 部署 helper 和可验证 ZIP 打包。
- 共享核心：C++26 modules 识别流水线、Rust C ABI、XChaCha20-Poly1305 封套、外部 i18n 资源、统一 `assets/` 布局和 Xmake 构建。

Windows 锁屏认证的当前数据流是：

```text
Credential Provider
  -> 受 ACL 保护的命名管道
  -> LocalSystem 认证服务
  -> 目标用户会话中的 recognition agent
  -> 活体与人脸比对
  -> 一次性登录凭据
  -> Windows LSA
```

`su_app.exe` 不参与锁屏认证；它仅在交互会话中通过命名管道请求服务完成档案管理和设置操作。早期的 GUI UDP 识别服务器已删除。

## 2026-09-05 验证快照

- Linux x86_64 Release 主构建成功。
- Xmake 共执行 14 个 test case，全部通过；损坏的 DMS/Matugen 调色板现在保留最后一个有效主题并报告被拒绝的来源。
- Rust core：42 个单元测试全部通过。
- Windows Rust Credential Provider（MinGW + Wine）：41 通过，1 忽略。忽略项依赖 Wine 未实现的 `CredIsProtectedW`，必须在真实 Windows 上验收。
- Windows 全量 Release（GUI、部署 helper、认证服务、识别 agent、密码工具和 Rust Credential Provider）通过 MinGW 交叉构建；按 SID 限流测试在 Wine 下通过。
- 同一工作区新构建产物的 Windows 未签名开发 staging 通过依赖、清单与文件哈希验证，但它不能部署，也不视为发布包。正式 ZIP 需要受信任的发布证书。
- Linux 包校验在本机因缺少 `patchelf` 未执行完成；CI 工作流已安装该依赖并覆盖 Linux tar 包构建与验证。

## 发布前阻塞项

主要安全与生命周期修复已完成：主题回归、Linux 管理授权 capability、Windows 签名输入与句柄复制、按 SID 管道限流、Linux 安装生命周期、crate 锁文件与 CImg 版本固定、FFI 警告和构建验证工作流均已落实。依赖与发布流程仍有下列阻塞项。

剩余阻塞项按顺序执行：

1. 将 SeetaFace6Open 主仓库及递归子模块固定到已验收的 commit；当前本地包配方直接克隆上游 HEAD，干净构建不可复现。
2. 在托管构建验证工作流上确认 Linux、Windows 交叉测试和临时证书签名包三个 job 全绿，并为 `main` 配置必需检查；当前分支保护尚未启用。
3. 使用正式受信任且带时间戳的 Windows code-signing 证书，从同一提交生成并验证 Windows ZIP；不得复用旧产物。
4. 在含 `patchelf` 的干净环境从同一提交生成并验证 Linux 包，并补做目标发行版原生包管理器的安装、升级、回滚与卸载验证。
5. 按 `release_acceptance_checklist.md` 完成 Windows 与 Linux 真实系统矩阵，保留日志和版本证据。
6. 仅在上述证据齐全后创建 `v2.3.0` tag 和 GitHub Release；发布后由独立工作流上传正式构建产物和校验和。

## 必须的现场验收

### Windows

- 真实摄像头与真人活体的录入、锁屏、解锁和冷启动闭环。
- 系统密码 tile、辅助功能、取消、错误提示、切换用户和服务停止时的安全回退。
- 物理 TPM2、无 TPM 的 machine-DPAPI 回退、BitLocker、密钥不可用和服务重启。
- 本地账户、Microsoft 账户、密码修改后的 stale 凭据和离线服务。域/Entra 账户当前明确拒绝，除非单独完成威胁建模与验收。

### Linux

- 在 Arch Plasma、Fedora Plasma/GNOME、Debian/Ubuntu GNOME 和 openSUSE Plasma 上验证真实 greeter、锁屏、密码回退和恢复控制台。
- 在 Fedora SELinux enforcing 下验证 PAM 到 control socket 的访问；如需策略，只发布最小的版本化规则。
- 验证 GUI 预览后锁屏、休眠/恢复、摄像头热插拔、两个本地账户新鲜录入，以及 daemon/模型/档案故障时的密码回退。
- 验证安装、升级、降级拒绝、卸载和 PAM 回滚的完整生命周期。

## 可选后续

- 自动检测 `pam_systemd_loadkey` 与引导密钥，在条件成立时为 KWallet/GNOME Keyring 提供人脸成功分支。
- 可恢复的非敏感引导进度、脱敏诊断导出以及更完整的取消/错误可访问性。
- Zig helper 和 SIMD 优化继续默认关闭，不阻塞第一版发布。

## 文档维护规则

- 架构和发布状态以本文、两份 README 和当前代码为准。
- 设计计划中的历史阶段可以保留，但已被后续架构取代的内容必须明确标记为历史。
- 正式发布前必须用当前提交重跑构建、测试和包校验，不得仅依赖 `build/` 中的旧产物。
