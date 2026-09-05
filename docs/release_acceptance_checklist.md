# Release Acceptance Checklist

更新日期：2026-09-05。以下项目必须按顺序执行；自动化通过不能替代真实登录栈验收。

## 1. 固定候选提交

- [ ] 工作区无非预期修改，记录 commit SHA、版本号和依赖锁定文件。
- [ ] 推送候选分支，禁止复用其他提交生成的 `build/` 或旧安装包。

## 2. 自动化流水线

- [ ] Linux job：全构建、14 个 Xmake 测试、Rust 42 个测试和 tar 包校验全部通过。
- [ ] Windows cross job：helper/service/password/限流目标构建，Wine helper smoke、限流和 Rust CP 测试通过。
- [ ] Windows package job：临时 CI 证书完成所有 PE Authenticode、manifest CMS、文件集合与 SHA-256 校验。

## 3. 正式候选包

- [ ] 用受信任的 Windows code-signing 证书从候选提交生成 ZIP，并在干净 Windows 中验证签名链。
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
