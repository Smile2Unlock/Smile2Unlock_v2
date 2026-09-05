# Release Acceptance Checklist

更新日期：2026-09-05。以下项目必须按顺序执行；自动化通过不能替代真实登录栈验收。

## 1. 固定候选提交

- [x] 版本文件已从 `2.2.0` 更新到尚未使用的 `2.3.0`；尚未创建 `v2.3.0` tag 或 Release。
- [ ] 将 SeetaFace6Open 主仓库及递归子模块固定到已验收的 commit，确保干净构建不会随上游 HEAD 漂移。
- [ ] 工作区无非预期修改，记录 commit SHA、版本号和依赖锁定文件。
- [ ] 推送候选分支，禁止复用其他提交生成的 `build/` 或旧安装包。

## 2. 自动化流水线

- [ ] Linux job：全构建、全部 Xmake 测试、Rust 测试和 tar 包校验全部通过。
- [ ] Windows cross job：helper/service/password/限流目标构建，Wine helper smoke、限流和 Rust CP 测试通过。
- [ ] Windows package job：在 main、`v*` tag 或手动运行中，以临时自动化证书完成所有 PE Authenticode、manifest CMS、文件集合与 SHA-256 校验。

## 3. 正式候选包

- [ ] 配置 GitHub `release-signing` environment：`WINDOWS_SIGN_CERTIFICATE_BASE64`、`WINDOWS_SIGN_KEY_BASE64` secrets 和 `WINDOWS_TIMESTAMP_URL` variable；限制为受信任的 release tag。
- [ ] 用受信任的 Windows code-signing 证书和可信时间戳从候选提交生成 ZIP，并在干净 Windows 中验证签名链。
- [ ] 在含 `patchelf` 的干净 Linux 环境生成并验证 Arch、DEB、RPM 或计划发布的实际格式。
- [ ] 记录包 SHA-256、构建日志、签名身份和构建环境。

## 4. Windows 真实系统验收

- [ ] 本地账户：录入、锁屏、冷启动、解锁、取消、错误提示、密码修改和离线服务。
- [ ] Microsoft 账户：登录名规范化、密码轮换和断网；域/Entra 继续保持拒绝。
- [ ] 系统密码 tile、辅助功能、切换用户、服务停止和组件损坏时仍可安全登录。
- [ ] 物理 TPM2、无 TPM 的 machine-DPAPI 回退、BitLocker、密钥不可用和服务重启。
- [ ] 安装、升级、卸载、注册表/服务/Provider DLL 清理及重启后残留。

## 5. Linux 真实系统验收

- [ ] Arch Plasma、Fedora Plasma/GNOME、Debian/Ubuntu GNOME、openSUSE Plasma：登录、锁屏和密码回退。
- [ ] 活动本地会话的 Polkit 管理授权：成功、取消、超时、重放、错误会话和 helper 崩溃。
- [ ] Fedora SELinux enforcing 下 PAM/control socket；若需要策略，仅接受最小版本化规则。
- [ ] 摄像头热插拔、休眠恢复、两个本地账户、模型/daemon/档案故障和恢复控制台。
- [ ] 原生包安装、升级、降级拒绝、断电恢复、重复卸载和全部 PAM 回滚。

## 6. 发布门禁

- [ ] 所有失败都有 issue、修复提交和重跑证据；不得以“可手工恢复”跳过密码回退失败。
- [ ] 文档、版本号、包清单和签名均指向同一候选提交。
- [ ] 完成以上项目后再创建 tag、发布说明和正式产物。
- [ ] GitHub Release 发布后，确认独立发布工作流上传 Linux tar.gz、Windows 签名 ZIP 和 `SHA256SUMS`。
