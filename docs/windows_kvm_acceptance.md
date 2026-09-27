# Windows KVM 实机验收记录

验收日期：2026-09-28。环境：KVM 虚拟机 `win10`（Windows 10 21H2, build 19044, x64，SPICE 显示，无直通物理摄像头），管理员 `Administrator`。被测产物：`smile2unlock-2.3.0-windows-x86_64.zip`（内部自签名证书签名，PIN `SMILE2UNLOCK_PINNED_SIGNER_SHA256` 校验通过）。

## 测试方法

- **虚拟摄像头**：Windows 侧摄像头栈只认 Media Foundation 枚举（`MFEnumDeviceSources` 走 PnP 设备接口），OBS 的 DirectShow 虚拟摄像头对 MF 不可见（`MFCreateVirtualCamera` 是 Windows 11 API）。改用**驱动级虚拟摄像头**（注册于 KSCATEGORY_CAPTURE）循环播放一张公有领域人脸照片。
- **档案引导**：宿主机用 `su_embed_probe`（#72）对同一张照片提取 1024 维 embedding，经管道 `enrollfull` 写入加密档案。
- **管道客户端**：C# 测试 harness（`[Reflection.Assembly]::LoadFrom` 装入现有 PowerShell，规避 VM 上新 .NET exe 的 SxS 启动失败）通过计划任务分别以 Administrator（session 1）和 SYSTEM 运行。

## 结果矩阵

| 验收项 | 结果 |
| --- | --- |
| 签名 ZIP 部署 + verify（信任链、布局、清单） | PASS |
| 服务安装并运行（LocalSystem，`Smile2UnlockAuthService.exe`） | PASS |
| Credential Provider CLSID + LogonUI 注册 | PASS |
| 密码录入 `kStore`（真实 LSA 密码校验） | PASS |
| 档案录入 `store`+`enroll`（加密档案 + 管理令牌） | PASS |
| `kSetRecognitionSettings`（阈值 0.65、超时 5s 等） | PASS |
| 识别 agent 提取特征（status=0，feature_count=1024，liveness=1） | PASS |
| SYSTEM `kAuthenticateAndPrepare` 端到端（比对通过，返回一次性密钥） | **AUTH-OK** |
| 非 SYSTEM 调用者执行 authenticate 被拒（`kAccessDenied`） | PASS |
| 客户端断开终止 agent（#70 `ClientDisconnectWatcher`） | PASS，见下 |

断开终止的量化数据（agent 出现后计时）：对照组正常完成识别存活 1746 ms；实验组在客户端被杀后 **约 220 ms** 内终止（自身预算 5 s 未用尽），无孤儿进程。

## 发现的问题

1. **产品 bug（#71 已修复）**：两个管道客户端（Rust CP 的 `connect_pipe`、C++ 管理端的 `CallNamedPipeW`）都没有请求模拟级管道句柄（`SECURITY_SQOS_PRESENT | SECURITY_IMPERSONATION`），服务端 `ImpersonateNamedPipeClient` 得到匿名令牌，真实 Windows 上所有连接在读取请求前即被拒（ERROR_NO_IMPERSONATION_TOKEN, 1368）。Wine 测试未覆盖此问题，因为 Wine 假服务器不做调用方身份解析——这是 Wine 覆盖的已知盲区。服务端拒绝日志（#72）正是定位该问题的关键证据。
2. **测试方法限制**：驱动级虚拟摄像头为试用版带水印；本次验收活体检测关闭（`LivenessEnabled=0`），阈值 0.65 下的活体路径未在实机覆盖。

## 遗留

- 物理摄像头（UVC 直通）下的真实取流、活体与拒真/认假率未在本环境覆盖。
- LogonUI 实际解锁会话（登录屏幕交互）仍属手工项：CP 已注册且管道链路全绿，但 KVM 会话切换未自动化。
