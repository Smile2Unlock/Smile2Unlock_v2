# Release Acceptance Checklist

更新日期：2026-09-06。以下项目必须按顺序执行；自动化通过不能替代真实登录栈验收。

## 1. 固定候选提交

- [x] 版本文件已从 `2.2.0` 更新到尚未使用的 `2.3.0`；尚未创建 `v2.3.0` tag 或 Release。
- [x] SeetaFace6Open 已固定到验收构建使用的 `a32e2faa0694c0f841ace4df9ead0407b78363c6`，递归子模块由该提交的 gitlinks 固定。
- [x] 候选分支已推送；`src/core-rs` 与 `src/platform/windows/credential_provider_rs` 的 `Cargo.lock` 均在版本控制中，Xmake 与 CI 使用 `--locked`。
- [x] 本地干净环境验证记录（2026-09-06，`scripts/local-ci.sh` + act）：stable rustc 解析为 1.98.1 (48a229cea 2026-09-01)；Linux job 14/14 测试通过并产出已验证 tar.gz；windows-package 产出临时证书签名 ZIP 并通过校验；windows-cross 三组 Wine 测试全部通过。候选提交 SHA 以合并到 `main` 的提交为准。

## 2. 自动化流水线

- [ ] Linux job：全构建、全部 Xmake 测试、Rust 测试和 tar 包校验在托管 runner 通过（本地已绿，待托管重放）。
- [ ] Windows cross job：helper/service/password/限流目标构建，Wine helper smoke、限流和 Rust CP 测试在托管 runner 通过（本地已绿，待托管重放）。
- [ ] Windows package job：在 main、`v*` tag 或手动运行中，以临时自动化证书完成所有 PE Authenticode、manifest CMS、文件集合与 SHA-256 校验（本地已绿，待托管重放）。

## 3. 正式候选包

- [x] GitHub `release-signing` environment 已创建，仅允许 `v*` tag，并配置 `WINDOWS_TIMESTAMP_URL` variable。
- [ ] 将正式证书链和私钥分别写入 `WINDOWS_SIGN_CERTIFICATE_BASE64`、`WINDOWS_SIGN_KEY_BASE64` environment secrets；私钥不得进入仓库、Actions artifact 或 GitHub Release asset。
- [ ] 用受信任的 Windows code-signing 证书和可信时间戳从候选提交生成 ZIP，并在干净 Windows 中验证签名链。
- [x] 干净 Linux 容器（含 `patchelf`）已从候选源码构建并验证 tar.gz、pacman、deb、rpm 四种格式；pacman 安装生命周期（安装、版本检查、重装、卸载）在全新 Arch 容器通过。容器验证发现：Arch 构建的 deb/rpm 二进制要求 GLIBC_2.38 与 GLIBCXX_3.4.36，Debian bookworm 无法配置、trixie/Fedora 42 可安装但运行需 GCC 16 运行时——正式 Debian/Fedora 包需按发行版重建后再次验收。
- [ ] 记录包 SHA-256、构建日志、签名身份和构建环境（以最终候选提交为准）。

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
