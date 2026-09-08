# Smile2Unlock 当前状态与剩余工作

更新日期：2026-09-08。本文是当前实现状态的入口；其他 `*_plan.md` 保留设计背景和历史记录，其中未勾选项不一定代表当前代码尚未实现。

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

## 2026-09-08 验证快照

- 三个 readiness job 用 `act` + Docker 重新在本地干净环境全部跑绿：Linux 全构建 + 42 个 Rust 单元测试 + 14 个 Xmake test + tar.gz 打包校验；Windows 特权组件 MinGW 构建 + 三组 Wine 测试；Windows 全量 Release + 临时证书签名 ZIP 验证。
- 加固了工作流对慢/代理 registry 的容忍度：顶层 `CARGO_NET_RETRY`、`CARGO_HTTP_TIMEOUT`、`CARGO_HTTP_LOW_SPEED_LIMIT`、`CARGO_HTTP_MULTIPLEXING=false`，并为三个 job 增加 `~/.cargo/registry`、`~/.cargo/git` 缓存。此前本地 windows-package 曾因 cargo 下载超时（`transfer too slow`）失败。
- `scripts/local-ci.sh` 增加持久化 `s2u-cargo-registry` 与 `s2u-pacman-cache` 卷，使重试从中断处继续，避免每次重新下载完整 Slint 依赖图与系统包。
- 修复 `publish-release-artifacts.yml` 中从未同步的 readiness 修复：缺少 `XMAKE_ROOT=y`（容器内 root 运行 xmake 会拒绝启动）、`nodejs`（容器 job 内的 JS actions）与 `libinput`（预编译 Slint 运行时链接依赖），并且 `osslsigncode` 不在官方仓库、改为从固定上游 2.14 源码构建。
- 托管 GitHub runner 上以 `workflow_dispatch`（run 34233577169）重放同一工作流：Linux、Windows 交叉测试与临时证书签名 Windows ZIP 三个 job 全部通过；PR run 34233566758 的 Linux 与 Windows 交叉两个 job 亦通过。三个 job 的 `Cache Cargo registry` 步骤均生效。

## 2026-09-06 验证快照

- 三个 readiness job 已用 `act` + Docker 在本地干净环境全部跑绿（见 `scripts/local-ci.sh`）：Linux 全构建 + 14 个 Xmake test + tar.gz 打包校验；Windows 全量 Release + 临时证书签名 ZIP 验证；Windows 特权组件 MinGW 构建 + 三组 Wine 测试。本地运行修正了六个此前必然失败的问题（rustup 组件参数、容器 root 需要 `XMAKE_ROOT=y`、zig 工具链误声明、Slint 代码生成时机、缺 `libinput`、`osslsigncode` 与新版 mingw GCC 不在 Ubuntu 源内），修复均已合入工作流。
- Rust core 42 个单元测试通过；stable channel 实际解析 rustc 1.98.1 (48a229cea 2026-09-01)。
- Linux 四种包格式（tar.gz、pacman、deb、rpm）在干净 arch 容器内构建并通过 `verify-package.sh` 完整校验。
- 原生包容器生命周期验证：Arch `pacman -U` 安装、版本检查、重装（pre_upgrade 路径）与卸载全部通过；deb/rpm 在 Debian trixie / Fedora 42 上可安装但运行时二进制要求 GLIBC_2.38 与 GLIBCXX_3.4.36（GCC 16 运行时），Debian bookworm 上 preinst 即失败。结论：当前 Arch 构建的 deb/rpm 仅适用于同代工具链发行版；正式支持 Debian/Fedora 需要按发行版构建。
- 托管 GitHub runner 的同工作流运行与 `main` 分支保护仍待完成（合并 PR 后确认）。

## 发布前阻塞项

主要安全与生命周期修复已完成：主题回归、Linux 管理授权 capability、Windows 签名输入与句柄复制、按 SID 管道限流、Linux 安装生命周期、crate 锁文件与 CImg 版本固定、FFI 警告和构建验证工作流均已落实。

剩余阻塞项按顺序执行：

1. 为 `main` 配置必需检查。托管 runner 上三个 job 已全部通过（`workflow_dispatch` run 34233577169），本地 act 亦全绿；合并 PR 后启用分支保护即可。
2. 使用正式受信任且带时间戳的 Windows code-signing 证书，从同一提交生成并验证 Windows ZIP；不得复用旧产物。
3. 在含 `patchelf` 的干净环境从同一提交生成并验证 Linux 包（tar.gz 已在本地 CI 覆盖）；目标发行版原生包的安装、升级、回滚与卸载需在真实发行版上验收——容器验证已给出工具链兼容矩阵，deb/rpm 正式包需按发行版重建。
4. 按 `release_acceptance_checklist.md` 完成 Windows 与 Linux 真实系统矩阵，保留日志和版本证据。
5. 仅在上述证据齐全后创建 `v2.3.0` tag 和 GitHub Release；发布后由独立工作流上传正式构建产物和校验和。

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
