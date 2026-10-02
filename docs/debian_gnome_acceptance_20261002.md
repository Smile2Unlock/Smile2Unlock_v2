# Debian 13 / GNOME / GDM 验收（2026-10-02）

初轮验收发现 Arch 构建的 DEB 不能直接在 Debian 13 运行，需隔离测试运行库才能继续桌面测试；同时修复 D-Bus 激活文件缺少 `Exec` 的问题。随后完成统一运行库打包，**新 DEB 已能直接在 Debian 13 运行**。初轮产物与隔离措施保留在下文，不能混用其结论或哈希。没有新增 CI job/check，也没有提交一次性脚本、密码或密钥。

## 后续：统一运行库打包

tar.gz、Arch、DEB、RPM 共用递归依赖收集。独立可执行程序使用包内配套 glibc/加载器，实际收集了 35 个系统运行库，包括 libstdc++、libgcc、OpenMP 及压缩、字体、输入等依赖；Slint/SeetaFace 和已有 CPU 变体也保留。GPU 驱动及其分发层、系统 PAM 和系统命令仍由发行版提供。运行库清单记录原始哈希和来源包版本，并附带可用的版权/许可文件。PAM 静态链接 C++/GCC 运行库，仅导出两个 C 入口；没有私有 RPATH，不向 GDM 进程加载私有 libc。该构建的 PAM 最低要求为宿主 glibc 2.38，原生包明确声明此依赖，因此 **Debian 12 的 glibc 2.36 仍不满足要求**。

在已删除初轮 `/opt/s2u-test-runtime` 和全部测试 drop-in 的同一 VM 中，使用默认 DEB 目录直接安装/重装新包；系统 libc6 仍为 2.41，包内独立程序使用配套的 2.44 运行库。实际结果：

| 检查 | 结果 |
| --- | --- |
| helper 版本及实际 D-Bus 激活/部署 | 可直接运行；初始化、GDM Plan/Apply/Inspect 成功，既有 systemd 沙箱保留 |
| 真实 daemon | 直接通过原始 unit 启动，未添加测试 drop-in；监听 control socket |
| 实际 GUI | Slint 窗口在 GNOME/Wayland 显示，模型 Ready、提示无摄像头；本次通过用户 transient unit 启动，未验收会话锁监控或完整 GUI 操作 |
| PAM 依赖 | Debian `ldd` 只解析到系统 libc/libm/libpam 等，不需要 libstdc++/libgcc；链接导出仅为两个 PAM 入口 |
| 实际 GDM / GNOME 锁屏，受控 accepted | 请求来自 uid 0，均无需输入密码就进入桌面/解锁 |
| 实际 GNOME 锁屏，受控 unavailable | 保持锁定，正确密码回退成功 |
| 实际 GDM / GNOME 锁屏，真实 daemon | 未录入档案返回 rejected，正确密码回退成功 |
| 现有 PAM 集成测试 | 直接运行重新构建的 `su_pam_integration_test` 通过 |
| 四格式打包 | C++ ELF fixture 的现有 smoke 和真实 Release 四格式均通过私有 loader/闭包/导出/清单校验；没有增加 CI job/check |

最终 DEB SHA-256：`b2c5628a9c1509109cee3ed1ddb2dfd709c54fe82e6236482eeee757045d1233`。APT 卸载不再需要提供测试运行库环境变量；7 个 PAM 哈希恢复或保持原样，模块、子栈和包内 libc 均删除，服务 inactive。最终原始证据和四格式产物 SHA-256 保存在本地 `bundled-*` 日志及 `bundled-package-sha256.txt`。没有摄像头/真人识别、其他架构或新 bundle 的完整 Fedora 桌面复测。此结果适用于本次具体产物；未来构建的 PAM 基线从 ELF 重新计算，不能由 Debian 13 通过推断任意旧系统可用。

## 环境与产物

使用 [Debian 官方 trixie Cloud 镜像](https://cloud.debian.org/images/cloud/trixie/latest/) `debian-13-generic-amd64.qcow2`，官方 SHA-512 校验通过：
`a733e7d49442a03e70d03e4eb5aaf3967f3efc69ef70952f9bb10fc1ee2c4876eb95956b5ad2d31350e5fada768feb651352535fb8cd1233f61998a5a7d2e93c`。

在外置磁盘创建独立 KVM overlay，4 vCPU、6 GiB RAM、30 GiB 稀疏磁盘。通过 `apt-get --no-install-recommends` 安装最小 GNOME/GDM 及必要依赖；没有安装完整桌面任务包。AppArmor 内核支持为启用状态，没有修改其策略；未启用 SELinux。

| 组件 | 实测版本 |
| --- | --- |
| Debian / 内核 | 13.7 / 6.12.107+deb13-amd64 |
| GNOME Shell | 48.7-0+deb13u2 |
| GDM | 48.0-2（实际 unit 为 `gdm.service`） |
| GNOME Session | 48.0-1+deb13u1，实际桌面会话为 Wayland |
| libc6 / libstdc++6 | 2.41-12+deb13u4 / 14.2.0-19 |
| libpam0g | 1.7.0-5 |

原生二进制沿用跨发行版验收从 `053a1c44aabc4552a0df46423a7ae7c34f530d49` 构建的 Release；该提交到当前基线 `3c343b4a3dbcb43eb74d877a5efde5718288812b` 的 `src/` 与 `xmake.lua` 无变化。最终 DEB 在当前基线上加入下述 D-Bus 修复后重新打包，版本为 2.3.1。

两个 DEB 均显式使用 `--pam-module-dir /usr/lib/x86_64-linux-gnu/security`；本次没有验证或修正默认 DEB 的 PAM 安装目录。

| DEB | SHA-256 |
| --- | --- |
| 修复 D-Bus 激活前 | `68bf9b6c7c60c98c1e6ff6c3c015f555c4da87594b38a0db65a757c444909770` |
| 初轮 D-Bus 修复包（尚未统一打包运行库） | `239a1c6f4c6961c38013d896a6c38a98457d92e83e798d97f0fd893b296e4ae5` |

## 初轮实际发现与测试隔离

### 原生运行库仍不兼容

APT 安装成功，但部署 helper 缺少 `GLIBCXX_3.4.35` / `.36`。仅补测试用私有 C++ 运行库后，真实 daemon 及 SeetaFace 依赖仍缺少 `libm` 的 `GLIBC_2.43`。因此不能把下面的桌面链路通过解释为原始 DEB 已支持 Debian。

为继续验证 PAM/GDM，在 guest 的 `/opt/s2u-test-runtime/` 放置私有 `libstdc++`，给测试中的 helper/GDM 添加 systemd 环境 drop-in。GNOME reauth worker 起初仍无法加载 PAM 模块，随后仅在测试副本中用 `patchelf` 添加该目录的 RPATH，才通过锁屏认证。这个 PAM 模块与包内原文件不同，是测试隔离措施，没有修改产品源码。

真实 daemon 另通过私有 loader 与配套 libc/libm/libstdc++/libgcc 启动，全部位于 `/opt/s2u-test-runtime/daemon/`；没有替换 Debian 系统运行库。真实 daemon 启动及重启后均监听 control socket，未录入用户返回 `rejected`。卸载时也给维护脚本提供私有 C++ 运行库，因此其生命周期结果同样有此条件。

### D-Bus 激活文件缺少 Exec

Debian 的 `dbus-daemon` 未将原有 `io.github.smile2unlock.Deployment1.service` 加入 `ListActivatableNames`，实际调用报 `ServiceUnknown`；`ReloadConfig` 不能解决。

在文件中补入 `Exec=/bin/false` 后，服务进入可激活列表，实际系统 D-Bus 的 `InitializeRuntime`、`PlanDesktopIntegration`、`ApplyDesktopIntegration` 和 `Inspect` 均成功。这个取值沿用 Debian 的 systemd D-Bus 服务做法：由 `SystemdService=su-deploy-helper.service` 启动；未启用 systemd 激活时直接执行分支失败，避免绕过服务沙箱。激活文件字段参见 [D-Bus 规范](https://dbus.freedesktop.org/doc/dbus-specification.html#message-bus-starting-services)。

最终修复 DEB 同版本重装后，受管理 GDM 集成保留；停止 helper 再调用 `Inspect` 也能重新通过 D-Bus 激活。服务仍为 `ProtectSystem=strict`、`NoNewPrivileges=yes`。

## 实际桌面结果

通过 QMP 键盘输入操作真实登录和锁屏界面，以截图、`loginctl` 和服务日志交叉确认。受控 broker 只返回协议结果，不进行识别；仅在隔离 VM 临时替换真实服务。

| 路径 | 结果 |
| --- | --- |
| 安装前 Debian 原生密码登录 | 进入 GNOME Wayland 桌面 |
| GDM，受控 `accepted` | 选择用户后即发起 PAM 请求，来自 uid 0；未输入密码即进入桌面 |
| GNOME 锁屏，受控 `accepted` | 唤醒认证界面后请求来自 uid 0；`LockedHint` 从 yes 变为 no，回到桌面 |
| GNOME 锁屏，受控 `rejected` | 错误密码 `x` 后仍锁定；正确密码解锁 |
| GDM，受控 `unavailable` | 进入密码输入界面，正确密码可登录 |
| 重启 VM 后 GDM，真实 daemon | 请求到达真实服务，因未录入档案返回 rejected；正确密码进入桌面 |
| GNOME 锁屏，真实 daemon | 同样收到未录入档案的 rejected；保持锁定，正确密码后 `LockedHint=no` |
| 最终 DEB 重装及 APT 卸载 | 重装保留集成；卸载回滚成功，模块与生成子栈删除，服务 inactive |
| 卸载恢复 | 7 个 PAM 文件 SHA-256 与基线完全相同；删除私有运行库/drop-in 并重启 GDM 后，原生密码登录仍进入桌面 |

PAM 转换只将 `gdm-password` 的 `@include common-auth` 换成专用子栈，`pam_nologin`、禁止 root 登录、account/session/password 规则保留。恢复检查覆盖 `gdm-password`、`gdm-autologin`、`gdm-launch-environment`、`common-auth`、`common-account`、`common-session`、`common-password`。

最小 GNOME 环境未安装可选的 `pam_gnome_keyring.so`，安装前后均存在该模块的缺失日志；没有验收 Keyring 解锁或钱包 token。没有摄像头、人脸档案或真人识别；受控成功只证明 PAM/IPC/桌面认证分支。界面触发 PAM 请求不代表采集、活体、自动重试或真人冷启动登录已验收。GUI/交互 Polkit、完整 Debian 桌面、休眠恢复及 Debian 原生重编译也未覆盖。

## 本地证据与保留环境

忽略目录 `build/debian-gnome-acceptance-20261002/` 保留原始日志、截图、运行库哈希和一次性脚本。关键证据：

- `gdm-baseline-config.log`、`gdm-baseline-password-login.log`、`deb-native-install-runtime.log`
- `dbus-reload-debug.log`、`dbus-fixed-initialize.log`、`deb-fixed-reinstall.log`
- `gdm-controlled-success-and-lock.log`、`gdm-controlled-after-select.png`
- `gnome-lock-worker-debug.log`、`gnome-controlled-unlock-retry.log`、`gnome-controlled-unlock-after.png`
- `gnome-reject-wrong-password.log`、`gnome-reject-password-fallback.log`、`gdm-unavailable-and-real-service.log`
- `resume-state.log`、`real-gdm-fallback.log`、`real-gdm-desktop.png`、`real-gnome-lock.log`、`real-gnome-fallback.log`
- `deb-final-uninstall.log`、`pam-restore-verify.txt`、`cleanup-runtime.log`、`clean-password-login.log`、`clean-desktop.png`

VM 磁盘保存在 `/run/media/ation_ciger/本地磁盘/Smile2Unlock-tests/debian13/`。测试结束卸载被测包、清理受控 broker 与私有运行库，恢复原生 GDM 后关机，保留镜像和测试账号供复测。账号凭据只存于忽略目录并直接告知用户，不进入本文或 Git。
