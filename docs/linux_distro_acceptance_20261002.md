# Linux 跨发行版验收（2026-10-02）

被测源码：`053a1c44aabc4552a0df46423a7ae7c34f530d49`（main，已合并 #98）。
从该提交在 Arch 主机重新构建 Linux Release，启用 Slint 和 SeetaFace，生成版本 2.3.1 的 RPM/DEB。
本次只做本地验收，没有增加或修改 CI。结论适用于下面的具体产物，不代表其他发布标签或发行版原生构建已经通过。

## SELinux 修复与 Plasma Login Manager 复测

在上述提交基础上的 `codex/fedora-selinux-plasma-login` 工作树完成修复和复测。
新增版本 1.0 CIL 策略，给运行目录和 `control.sock` 使用专用 `smile2unlock_runtime_t` 标签，允许 `xdm_t` 搜索目录、写入该 socket，并保留 systemd 的沙箱挂载权限。
沿用 Fedora 已有的 `xdm_t` 到 `unconfined_service_t` 连接权限，不增加通用 `var_run_t` socket 写权限；`sesearch` 实际验证专用类型有写权限，通用类型没有。
策略在原生包安装/升级时自动加载到优先级 200，卸载时移除；64 位 RPM 的默认 PAM 路径修正为 `/usr/lib64/security`，`--format all` 的 RPM 输出也使用该路径。显式目录覆盖仍有效。

本阶段始终为 **SELinux Enforcing**，没有切换 Permissive：

| 检查 | 结果与证据 |
| --- | --- |
| 真实 `su-authd` 启动及重启 | active；新建目录和 socket 自动取得专用标签，原有 systemd 沙箱保留 |
| SDDM 实际界面，受控 accepted | 无效密码 `x` 仍进入 Plasma；服务收到 uid 0 的请求 |
| SDDM 实际界面，受控 rejected | 正确密码回退后进入 Plasma |
| Plasma Login Manager 6.7.5-1.fc44，受控 accepted | 实际 greeter 提交 `x` 后进入 Plasma；请求来自 uid 0，helper 运行于 `xdm_t` |
| Plasma Login Manager，受控 unavailable | 正确密码回退后进入 Plasma |
| 两种登录管理器，真实服务 | 请求到达真实 daemon；未录入档案返回 rejected，均可用正确密码登录 |
| Plasma Login Manager PAM 转换 | 识别 `/usr/lib/pam.d/plasmalogin`，生成 `/etc/pam.d/plasmalogin` 覆盖和专用子栈；厂商文件保持原样 |
| 最终 RPM 重装、重启和卸载 | 自动安装策略；重启标签正确；卸载移除策略、生成子栈及 Plasma Login 覆盖，服务 inactive；9 个 PAM 文件哈希与原始配置相同 |
| 现有四格式打包 smoke | tar.gz、pacman、DEB、RPM 全部通过；没有新增 CI 检查 |
| 源码安装脚本 `DESTDIR` | 可执行安装器正确暂存策略和管理脚本，没有加载主机策略 |

最终测试 RPM SHA-256：`af442a68ca017b5af28e972b1300803176a1d616f33b0b57f8be514217784e5a`。
CIL 策略 SHA-256：`5b06380c27722ad9a20e400e9e4e99c73e46ac5978f0e1b14e819da5fd8b566d`，已与最终 RPM 安装的文件比对。
源码安装脚本还补回了执行权限，供 GUI bootstrap 和文档中的直接命令使用；本次只验证其暂存路径，未重测 GUI bootstrap 授权弹窗。

本次实际测试的是独立的 `plasmalogin.service`，见 [Fedora 的 Plasma Login Manager 变更说明](https://fedoraproject.org/wiki/Changes/PlasmaLoginManager)。现有 PAM 转换代码无需新增该目标的实现。
受控 accepted 仍只证明 PAM/IPC/登录路径，不代表摄像头或真人识别通过；认证由提交表单触发，未证明自动识别触发或钱包解锁。
Debian ABI 问题仍未修复，其他 SELinux 策略/发行版也未因此获得验收。

新增本地证据（同一忽略目录 `build/distro-acceptance-20261002/`）：

- `fedora-policy-final-install-verify.log`、`fedora-policy-scope.log`、`rpm-selinux-build.log`
- `fedora-policy-sddm-success.log`、`fedora-policy-sddm-password-fallback.log`、`fedora-policy-sddm-real-fallback.log`
- `fedora-plasmalogin-apply.log`、`fedora-policy-plasmalogin-success.log`、`fedora-policy-plasmalogin-fallback.log`、`fedora-policy-plasmalogin-real-fallback.log`
- `fedora-policy-sddm-desktop.png`、`fedora-policy-plasmalogin-desktop.png`
- `fedora-policy-final-uninstall.log`、`selinux-packaging-smoke.log`、`selinux-source-stage.log`

## 修复前的结果

| 环境与产物 | 实际验证 | 结果 |
| --- | --- | --- |
| Fedora 44，默认 RPM | 安装、GUI、认证服务、D-Bus 部署、实际 KDE 锁屏 | 安装和 GUI 可用；PAM 模块目录错误，按模块名无法加载；密码回退可解锁 |
| Fedora 44，覆盖 PAM 目录的 RPM | PAM、KDE 锁屏、SDDM 登录、升级、卸载 | KDE 链路可用；SDDM 在 SELinux Enforcing 下无法连接认证 socket；密码回退可登录；升级保留集成，卸载恢复配置 |
| Debian 13 trixie，默认 DEB，容器 | APT 安装、部署 helper 启动 | 安装成功，启动缺少 `GLIBCXX_3.4.35` / `.36` |
| Debian 12 bookworm，默认 DEB，容器 | APT 安装、部署 helper 启动 | 安装成功，启动缺少 `GLIBC_2.38`、`GCC_13.0.0` 及较新 `GLIBCXX` |

这是修复前的快照：当时默认 RPM 有模块目录缺口；修正目录后仍有 SDDM 的 SELinux 访问缺口。这两个缺口现已按上节修复并复测。当前 Arch 生成的 DEB 仍不能直接用于上述 Debian 环境，需发行版原生构建或经过验证的兼容工具链。

## Fedora 环境与方法

使用 [Fedora 官方 Cloud 镜像](https://fedoraproject.org/cloud/download/)，在外置磁盘上建立独立 KVM overlay，不改主机 PAM 或其他虚拟机。
镜像为 `Fedora-Cloud-Base-Generic-44-1.7.x86_64.qcow2`，与官方校验文件比对通过：
`28680fe5b371a5a82ebf43a31926e086a168e59949d03969c5093e7071f90b7f`。

在最小 Cloud 系统中安装 SDDM、Breeze、Plasma、KScreenLocker 及必要运行依赖，关闭弱依赖安装；这不是完整 KDE Spin 镜像验收。
虚拟机为 4 vCPU、6 GiB RAM、30 GiB 稀疏磁盘；通过 QMP 键盘输入操作真实 greeter/锁屏，使用截图、loginctl、服务日志和系统调用记录验证结果。

| 组件 | 版本 |
| --- | --- |
| glibc | 2.43-2.fc44 |
| libstdc++ | 16.2.1-2.fc44 |
| PAM | 1.7.2-1.fc44 |
| SDDM | 0.21.0-13.fc44 |
| plasma-workspace / kscreenlocker | 6.7.5-1.fc44 |

### 默认 RPM 的模块目录

`packaging/linux/package.sh` 默认安装到 `/usr/lib/security`；Fedora 的 PAM 按名称在 `/usr/lib64/security` 加载。
实际 KDE 日志报错：`PAM unable to dlopen(/usr/lib64/security/pam_smile2unlock.so)`。
独立 PAM probe 按名称返回 28（Module is unknown）；指定已安装模块的绝对路径则可加载，真实服务因未录入人脸返回认证拒绝。
部署 helper 报告的 `managed` 只表示 PAM 配置已转换，不能据此断言模块可加载或登录已通过。

为隔离后续问题，仅使用已有参数重打 RPM：

```bash
packaging/linux/package.sh --format rpm \
  --build-dir build/linux/x86_64/release \
  --pam-module-dir /usr/lib64/security \
  --output-dir build/distro-acceptance-20261002/packages/fedora-lib64
```

这次覆盖仅用于验收，没有修改产品默认打包路径。同版本 `rpm -Uvh --replacepkgs` 后，两个受管理 PAM 集成仍保留，模块可按名称加载。

### PAM / KDE / SDDM

- 通过实际系统 D-Bus helper 执行初始化、Plan/Apply，确认 `/etc/pam.d/kde`、`sddm` 和专用子栈受管理；测试调用由 root 授权，未覆盖 GUI 的交互 Polkit 授权。
- 隔离的受控 IPC fixture 分别返回 accepted、rejected、unavailable。真实 PAM 模块在完整 `kde` / `sddm` 栈中可接受 accepted，失败结果会继续密码栈。
- 实际 KDE 锁屏：Enforcing 下，受控 accepted 请求来自 uid 1000；提交无效密码 `x` 后成功解锁，`LockedHint=no`。真实服务恢复后，未录入档案返回 rejected，正确密码仍可解锁。
- 实际 SDDM：Enforcing 下，受控 accepted 没有到达服务，无效密码无法登录；正确密码可登录。重启 SDDM 后复现，排除仅由旧进程导致的结果。
- strace 确认 SDDM helper 已读取受管理 PAM 文件、加载 `/usr/lib64/security/pam_smile2unlock.so`，但 `connect("/run/smile2unlock/control.sock")` 返回 `EACCES`。目录为 0755、socket 为 0666，SDDM helper 为 `xdm_t`，服务为 `unconfined_service_t`，socket 标签为 `var_run_t`。
- 同一配置和 fixture 临时切到 SELinux Permissive 后，实际 SDDM 请求来自 uid 0，accepted 后进入 Plasma 会话；随即恢复 Enforcing。该对照确认 SELinux 阻塞，但本次未取得可用的对应 AVC 记录，也未编写或安装策略。

受控 accepted **只证明 PAM / IPC / 桌面认证分支**，不是人脸识别成功。虚拟机没有摄像头和人脸档案，未验收真实采集、活体、自动触发、冷启动人脸登录或钱包解锁。
KDE/SDDM 的正向受控认证都由提交登录表单触发，本次没有证明打开界面后会自动发起识别。

### 生命周期

原始 RPM 安装、初始化、应用 PAM 集成、覆盖目录后的升级、原生 `rpm -e smile2unlock` 均实际执行。
卸载后 `/etc/pam.d/{sddm,kde,sddm-autologin,sddm-greeter,password-auth,system-auth}` 的 SHA-256 与安装前逐一相同，两个生成子栈和模块均已删除，`su-authd` inactive。
未测试降级、完整 KDE Spin、GNOME/openSUSE/Ubuntu 或物理摄像头。

## 产物与证据

| 产物 | SHA-256 |
| --- | --- |
| 默认 RPM | `9db36664b83c588cd027b1ea46f0c877dd03263ba29f52723c05220dc8e6c9ec` |
| 默认 DEB | `7f14338e528b5a8a7672aa28fad0601ff5d756a7602781b3c34594f35b0ab2e7` |
| `/usr/lib64/security` RPM | `ba6971665fc08e5bd9d73da04ac8d81a53a61c12f1667a100d1edbcf8abdf169` |

本地原始日志、截图和一次性验收脚本保存在 `build/distro-acceptance-20261002/`（忽略目录，没有加入 CI 或提交密码/密钥）。关键证据：

- `native-build.log`、`rpm-build.log`、`deb-build.log`、`rpm-lib64-build.log`
- `fedora-default-pam-test.log`、`fedora-module-loader-evidence.log`
- `fedora-lib64-upgrade.log`、`fedora-controlled-stack-success.log`
- `fedora-controlled-kde-success.log`、`fedora-controlled-kde-unlocked.png`、`fedora-lib64-real-kde-fallback.log`
- `sddm-syscalls-fresh.log`、`fedora-sddm-enforcing-control.log`、`fedora-sddm-permissive-control.log`
- `fedora-rpm-uninstall.log`、`debian-{trixie,bookworm}-install.log`

VM 磁盘保存在 `/run/media/ation_ciger/本地磁盘/Smile2Unlock-tests/fedora44/`，测试结束恢复 SELinux Enforcing、删除受控服务并卸载被测包后关机，保留镜像以便复测。

## 后续方向

1. 部署诊断还需验证模块是否能按实际服务使用的路径加载，避免仅凭 PAM 已转换就显示集成就绪。
2. 在所支持的 Debian/Fedora 原生环境构建发布包，声明实际 ABI 需求，避免安装成功但程序无法启动。
3. 在物理摄像头环境验收真人识别、自动触发、钱包解锁等尚未覆盖的功能。
