# Windows 安装包方案调研

调研日期：2026-09-28。目标：在现有 Linux 交叉编译 + osslsigncode 签名流水线之上，
为签名 ZIP 之外的交付形态增加一个标准 Windows 安装包（向导、UAC、卸载）。

## 结论

- **第一阶段用 NSIS 3 做薄壳安装器**：Linux 上原生构建（`makensis`），体积小、多语言
  向导（含简中）、admin 清单；安装动作复用现有的 `su_deploy_helper`
  （清单校验 + 服务安装 + CP 注册的已审计特权路径），安装器本身用现有
  osslsigncode + 内部证书签名。风险最低、CI 改动最小。
- **后续若需企业受管分发（GPO/SCCM/Intune）再补 WiX v6 MSI**：WiX v4+ 是 .NET 工具，
  支持 Linux 上构建 MSI；服务和 CP 注册表可写成原生 MSI 表（ServiceInstall /
  Registry），安装事务化。osslsigncode 原生支持 MSI 签名（`-add-msi-dse`）。
- **MSIX 不可行**：凭据提供程序是 LogonUI（容器外的系统进程）加载的 COM 对象，
  MSIX 的虚拟注册表对系统进程不可见，CP 无法在登录屏幕加载；服务与驱动支持也
  受限。排除。

## 本项目的安装约束

安装器必须完成（全部需要管理员权限）：

1. 解压文件到 `C:\Program Files\Smile2Unlock\`（含 `release-info.json` + `.p7s` 清单）
2. 校验每个文件的签名/哈希（信任模型：内部证书 PIN 编译进 `su_deploy_helper`，
   不能因为换了安装器就绕过）
3. 安装/替换 LocalSystem 服务（停旧、覆盖、启动）
4. 注册凭据提供程序（HKLM CLSID + LogonUI 键）
5. 写 ARP（"应用与功能"）卸载项；卸载时反向：注销 CP → 停删服务 → 删文件
   （用户数据 `C:\ProgramData\Smile2Unlock` 询问保留）

硬约束：构建必须能在 Linux CI 完成（现有 mingw + osslsigncode 全在 Linux）。

## 方案对比

| | NSIS 3 | WiX v6 (MSI) | Inno Setup | MSIX |
| --- | --- | --- | --- | --- |
| Linux 上构建 | 原生（makensis） | 可以（.NET 工具，v4+ 跨平台） | 需 Wine 跑 ISCC | 可以 |
| 服务安装 | 脚本/宏 或复用 helper | **原生表 ServiceInstall** | 脚本 | 受限（抬高最低 OS） |
| CP 注册表写入 | 脚本 或复用 helper | **原生 Registry 表** | 脚本 | **不可行（虚拟化）** |
| 卸载 | 手写脚本 | 标准化、事务化 | 手写脚本 | 系统托管 |
| 企业 GPO/SCCM | 一般 | **一等公民** | 一般 | 商店/侧载 |
| 多语言向导 | 内置（含 zhCN） | 需要 transforms | 内置 | 清单 |
| 安装包体积开销 | ~1.5 MB | ~1 MB + 运行时开销小 | ~2 MB | 容器开销 |
| 学习/维护成本 | 低（自有 DSL） | 中高（XML + MSI 概念） | 低（Pascal） | 中 |
| 与现有工具链 | osslsigncode 签 PE | osslsigncode 签 MSI（原生支持） | 签 PE | 需 signtool 体系 |

## 第一阶段设计：NSIS 薄壳 + 复用 su_deploy_helper

关键原则：**安装器不重新实现特权逻辑**。它只做 UI、解压和调用：

```text
smile2unlock-2.3.1-setup.exe (NSIS, Authenticode 签名)
  ├─ 向导页：许可(MIT + THIRD-PARTY-NOTICES) → 安装目录(默认 Program Files)
  ├─ 解压内嵌文件到临时目录（LZMA solid，SeetaFace 模型可显著压缩）
  ├─ 执行 bin\su_deploy_helper.exe --verify --register-cp --ensure-service
  │    （内部证书 PIN 校验 → 服务 → CP，全部走已审计代码路径）
  ├─ 成功：把文件落到安装目录、写 ARP 卸载项
  └─ 失败：回滚（删临时文件），显示 helper 的错误文本
卸载器（NSIS 生成的 uninstall.exe）
  ├─ su_deploy_helper --unregister-cp；停删服务
  ├─ 删除安装目录；ProgramData 询问保留
  └─ 清 ARP 项
```

实现要点：

- `RequestExecutionLevel admin` + `MultiUser` 关闭（per-machine 安装）
- 文件清单从打包脚本的 release-info.json 生成（`!system` 预处理或 xmake 任务生成
  `files.nsh`），保证 ZIP/安装器/清单三者永远同源
- 语言：`SimpChinese` + `English`，复用 `assets/i18n` 的术语
- CI：`pacman -S nsis`（或固定版本 zip 解包），在 windows-package job 后追加
  `makensis` 步骤，产物 `*-setup.exe` 与 ZIP 一起发布；两者都进 release 资产
- 版本从 2.3.0 提到 2.3.1 起步（ZIP 与安装器同版本号）

## 第二阶段（可选）：WiX v6 MSI 要点

- `dotnet tool install --global wix`（Linux 即可）；`wix build installer.wxs -o
  smile2unlock.msi`
- ServiceInstall/ServiceControl 表声明服务；Registry 表声明 CP 键；
  升级用 ProductCode/MajorUpgrade（2.3.1 → 2.4.0 自动替换）
- 完整性：MsiFileHash 表 + MSI 本身签名（osslsigncode `-add-msi-dse`）；
  安装后首启仍跑 `su_deploy_helper --verify` 保留 PIN 信任模型
- 自定义动作（如需要）用 mingw 编译的 CA DLL，避免 Windows-only 依赖

## 参考来源

- WiX v4+ 跨平台构建：[wixtoolset.org](https://wixtoolset.org)（v6 为当前稳定版）
- MSIX 限制：服务/驱动不支持、虚拟注册表对系统进程不可见 ——
  [msix-docs](https://github.com/microsoft/msix-docs/blob/master/msix-src/packaging-tool/know-your-installer.md)、
  [MSIX 中的 Windows 服务](https://www.advancedinstaller.com/introduction-to-windows-services-in-msix.html)
- osslsigncode MSI 数字签名扩展（`-add-msi-dse`）：osslsigncode 内置帮助
