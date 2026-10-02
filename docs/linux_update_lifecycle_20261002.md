# Linux 启动入口与更新生命周期（2026-10-02）

## 启动入口来源

宿主原生入口只有 `/usr/share/applications/smile2unlock.desktop`。用户截图中的
第二个 Smile2Unlock 和 Uninstall Smile2Unlock 来自用户目录下
`applications/wine/Programs/Smile2Unlock/`：两者的 `Exec` 都指向此前 NSIS
选项测试的 `build/nsis-options/wineprefix`，由 Wine 导出 Windows Start Menu 快捷方式。

本机这两个测试入口已移到忽略的审计目录备份，只保留原生 Linux 主程序入口。
该测试 prefix 的 `HKCU\Software\Wine\DllOverrides` 中禁用 `winemenubuilder.exe`，
没有改其他 Wine 环境或 Windows 默认快捷方式。后续 Wine 安装器测试使用
`WINEDLLOVERRIDES=winemenubuilder.exe=d`，参见
[Wine 开发者说明](https://www.winehq.org/pipermail/wine-devel/2010-July/085451.html)。
现有包校验器增加“恰好一个应用入口”约束，没有增加 CI job/check。

## 更新后刷新服务

此前 DEB/RPM 和 Arch 的安装后脚本只加载 SELinux 策略，源码安装器也仅刷新
systemd/D-Bus 配置；运行进程继续使用更新前的可执行程序和动态库。

新增共享 `/usr/libexec/smile2unlock/post-install`：文件替换完成后加载 SELinux
策略、执行 `daemon-reload`、刷新运行中的系统 D-Bus，再 `try-restart`
`su-authd.service` 与 `su-deploy-helper.service`。DEB/RPM post-install、Arch
post-install/post-upgrade 和非 DESTDIR 源码安装均调用它；手动安装 tar 文件树后
也可调用。`try-restart` 只重启运行中的服务，保留启用状态，参见
[systemd 文档](https://cgit.freedesktop.org/systemd/systemd/tree/man/systemctl.xml?id=99504dd4c13af7516a976fffc0f68e6f26d3faac)。
重启命令失败向安装器返回非零；无运行中 systemd 的离线安装跳过服务刷新。

## 本地验证

复用 Debian 13 / GNOME / GDM KVM 镜像和既有 Release 二进制，仅重新打包。
产物基于 `4f8a6d9` 加本次工作区打包修改，版本 2.3.1；DEB SHA-256：
`6e45fdaeef1f288a1413a34182c20253ce7221f93f7ab156e59cdfffd8f04d84`。

| 检查 | 结果 |
| --- | --- |
| 首次安装及未初始化时重装 | 两服务 inactive，认证服务 disabled，没有自动初始化 |
| 真实服务运行时同版本 APT 重装 | authd PID 1621→2060，helper PID 1484→2055；socket 和 D-Bus Inspect 正常，新进程无已删除运行库映射 |
| enabled 但手动 stop 后重装 | 两服务保持 inactive，enablement 保留 |
| disabled 但手动运行后重装 | 两服务 PID 换新，authd 保持 disabled |
| 管理中的 GDM/PAM 配置 | 更新前后 8 个文件哈希一致 |
| 卸载 | 7 个原生 PAM 文件恢复基线哈希，服务停止，入口移除，GDM 仍运行 |
| 现有四格式打包 smoke | tar.gz、Arch、DEB、RPM 校验通过，包括新钩子权限与单入口约束 |
| 源码安装 DESTDIR / Debian 离线容器 | 钩子正确暂存；离线安装不触发服务管理器 |

首轮验证脚本在 Type=simple 服务的 socket 就绪前提前检查，加入就绪等待后
继续完成运行中服务及卸载验证。原始日志、备份和一次性脚本保留在忽略目录
`build/linux-update-lifecycle-20261002/`，未提交测试账号凭据。VM 已卸载被测包、
恢复原生 PAM 并关机，磁盘保留。

本次未重复真人识别、摄像头或完整桌面登录验收；RPM/Arch 检查为打包验证，
不代表已经在这些发行版上执行更新生命周期。源码安装只执行了 DESTDIR 暂存，
其在线刷新行为由同一个在 Debian 实测的钩子提供。
