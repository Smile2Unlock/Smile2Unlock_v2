# Smile2Unlock 当前状态与剩余工作

更新日期：2026-09-03。本文是当前实现状态的入口；其他 `*_plan.md` 保留设计背景和历史记录，其中未勾选项不一定代表当前代码尚未实现。

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

## 2026-09-03 验证快照

- Linux x86_64 Release 主构建成功。
- Xmake 共执行 13 个 test case：12 通过，1 失败。失败项为 `su_theme_test/default`，损坏的 DMS 调色板原子替换会错误切换到内置主题，而不是保留当前主题。
- Rust core：42 个单元测试全部通过。
- Windows Rust Credential Provider（MinGW + Wine）：41 通过，1 忽略。忽略项依赖 Wine 未实现的 `CredIsProtectedW`，必须在真实 Windows 上验收。
- 现有 Windows 2.2.0 ZIP 通过当前包校验器。Linux 包校验在本机因缺少 `patchelf` 未执行完成，不视为包本身失败。

## 发布前阻塞项

1. 修复主题热更新回归，恢复 Xmake 测试全绿。
2. 为 Linux 录入、删除和迁移档案增加独立的 PAM 管理授权与短时 capability，不再只依赖同 UID 身份。
3. 加固 Windows 提权部署输入：验证签名 manifest/Authenticode，使用基于句柄且拒绝 reparse point 的复制流程，或切换到可审计的 MSI/WiX 安装器。
4. 恢复 CI，覆盖 Linux/Windows C++ 构建、Rust 测试、MinGW/Wine CP 测试、包校验和部署 smoke test。
5. 发布前从同一提交重新构建并验证 Windows ZIP，确认包内 `release-info.json` 覆盖全部载荷文件；不得复用 `build/packages/` 中的旧产物。
6. 为 Windows 认证管道增加按 SID 的连接/请求速率限制；现有 15 秒读取截止不能完全防止同步服务被占用。

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
