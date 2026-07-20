# Windows Rust Credential Provider 重写计划

## 目标

把当前 `CredentialProvider/` 下的 C++ Sample 重写为位于 `src/` 内的纯 Rust Windows Credential Provider。Rust DLL 只负责 Windows Credential Provider / COM 适配、登录凭据序列化和认证服务客户端，不把 GUI、摄像头、SeetaFace、Tauri、OpenCV 或大型异步运行时装进 LogonUI 进程。

本计划与 `docs/rewrite_master_plan.md` 和 `docs/credential_storage_encryption_plan.md` 配套使用。实现完成后，需要把主计划中的“C++ Credential Provider 适配层”更新为 Rust 适配层；在此之前不修改旧 Provider 的行为。

## 当前状态

- 现有 C++ Provider 和 Windows LocalSystem 认证服务仍是当前可构建路径。
- 加密 profile / Windows password envelope、TPM CNG provider、machine DPAPI fallback 的设计见 `docs/credential_storage_encryption_plan.md`。
- `po0uyan/memsafe` `v1.0.2` 的 Linux 测试已通过，但尚未完成 Windows 原生验证，也没有第三方安全审计。
- 本计划阶段只写文档，不替换当前 DLL、不修改注册表、不删除旧目录。

## 非目标

- 不直接复制 `FaceWinUnlock-Tauri` 或任何其他 Credential Provider 实现。
- 不在 Credential Provider DLL 内启动 pipe server、HTTP server、Tauri runtime、Tokio runtime 或摄像头线程。
- 不隐藏 Windows 自带密码 Provider，不把人脸 tile 变成通用密码输入框。
- 不在 `CPUS_CREDUI` 中读取或自动提交已保存 Windows 密码；该场景需要独立威胁建模后再决定。
- 不把 `MemSafe<String>`、`Vec<u16>` 或普通 `String` 作为长期密码容器。

## 目录与构建边界

新实现暂定目录：

```text
src/platform/windows/credential_provider_rs/
├── Cargo.toml
├── exports.def
└── src/
    ├── lib.rs
    ├── class_factory.rs
    ├── provider.rs
    ├── credential.rs
    ├── fields.rs
    ├── serialization.rs
    ├── event_sink.rs
    ├── pipe_client.rs
    └── secret_buffer.rs
```

约束：

- 产物为 `cdylib`，使用显式 `.def` 导出 `DllGetClassObject`、`DllCanUnloadNow`、`DllRegisterServer` / `DllUnregisterServer`（若安装器需要）。
- Windows-only crate 不参与 Linux target；公共协议和加密存储 API 通过已有 Rust core / 明确的跨平台协议复用。
- xmake 增加独立的 Windows Rust target，只负责调用 Cargo、复制 DLL 和 `.def` 需要的构建产物；不把旧 C++ 源文件混入新 target。
- 迁移期间保留旧 `CredentialProvider/` target，使用不同的输出目录和测试注册配置，避免覆盖当前可用安装。

## 设计基线

### COM 与 Credential Provider

按 Windows SDK 接口实现最小对象模型：

- `IClassFactory`
- `ICredentialProvider`
- `ICredentialProviderSetUserArray`
- `ICredentialProviderCredential`
- `ICredentialProviderCredential2`
- `ICredentialProviderCredentialWithFieldOptions`

实现要求：

- 使用 `windows` / `windows-core` 的接口和 `implement` 宏，所有 HRESULT、引用计数和 GUID 转换经过明确的边界函数处理。
- 不在 `DllMain` 做文件、注册表、锁、线程或网络操作。
- COM apartment 规则写入模块注释和测试；Credential Provider 回调不假设可跨线程直接使用。
- `ICredentialProviderEvents` 保存前必须进行 COM marshal，不能通过 `unsafe impl Send/Sync` 绕过线程模型。
- 所有外部输入、字段索引、用户数组、序列化长度和 HRESULT 都做边界校验，生产路径禁止无依据的 `unwrap()`。

### 认证服务 IPC

- Provider 只作为命名管道客户端连接 LocalSystem `Smile2UnlockAuthService`。
- 管道 ACL、服务端 token / SID 校验、一次性 session token、request id、LogonUI session 和目标 SID 必须同时绑定。
- Provider 不取得 master key，不打开 profile 文件，不执行加密解包；密码只能通过一次性 `prepare_logon_credential` 响应取得。
- 请求超时、服务不可用、session 不匹配和响应解析失败均安全失败，并保留系统密码 Provider。
- `ReportResult` 收到密码错误、密码过期或必须修改密码后，将对应 secret 标记为 stale；同一 request 不得再次自动提交。

### 登录场景

第一版只开放：

- `CPUS_LOGON`
- `CPUS_UNLOCK_WORKSTATION`

以下场景默认不读取保存密码：

- `CPUS_CREDUI`
- 密码修改、远程凭据或未验收的域 / Entra 账户流程

人脸验证成功后，Provider 只为当前已绑定的 SID 返回 Windows `KERB_INTERACTIVE_UNLOCK_LOGON` 所需的序列化数据。Provider 不提供独立的手工密码字段；系统自带密码 tile 始终可用。

## `memsafe` 采用方案

### 采用边界

`memsafe` 只作为密码和密钥的纵深防御，不改变现有 XChaCha20-Poly1305 / CNG / DPAPI 密钥模型：

- 认证服务 master key：评估 `Secret<32>`。
- Provider 一次性密码：使用固定容量 `Secret<N>`，内部保存 UTF-16LE 字节和有效长度。
- 业务层只依赖本地 `WindowsSecret<N>` 封装，不直接暴露 `memsafe` 的 guard 和平台细节。
- 所有 FFI 临时缓冲继续使用 `zeroize` / `SecureZeroMemory`，不能因为采用 `memsafe` 就取消清零。

### 生产门槛

在正式依赖前完成一个固定 commit 的 fork 或补丁分支：

1. 锁定 `v1.0.2`，记录 checksum / commit，禁止无审查的浮动版本。
2. Windows 封存态评估并优先改为 `PAGE_NOACCESS`；确认 `VirtualLock`、保护页切换和 LogonUI 环境兼容。
3. 移除 guard `Drop` 中的 `unwrap()`，改为不 panic 的 fail-secure 清理路径，并记录不可含秘密的诊断状态。
4. 添加 Windows stale-pointer、异常展开、线程交错、锁页配额和子进程 dump 测试。
5. 验证 UTF-16 输入不会先生成普通 `String` / `Vec<u16>` 副本。

若上述补丁不能在目标 Windows 版本上稳定通过，Provider 不得把 `memsafe` 当作安全保证；应退回经过审查的固定容量 `zeroize` 缓冲，并保留同样的生命周期和 FFI 约束。

## 分阶段实施

### Phase 0：冻结接口与安全样例

- [ ] 固定 Windows SDK、Rust toolchain、`windows` crate 和 `memsafe` fork 版本。
- [ ] 用最小 Rust `cdylib` 验证 x64 MSVC 原生构建、MinGW 交叉构建和 DLL 导出检查。
- [ ] 写 COM GUID / HRESULT / field descriptor 的纯内存单元测试。
- [ ] 完成 `WindowsSecret` 原型和 Windows 内存保护测试；不接入 LogonUI。
- [ ] 记录旧 C++ Provider 的 CLSID、字段布局、注册项和卸载行为，作为兼容基线。

### Phase 1：Rust COM 骨架

- [ ] 实现 `DllGetClassObject`、class factory、引用计数和生命周期。
- [ ] 实现 provider 的字段元数据、用户数组、usage scenario 和 credential object 创建。
- [ ] 实现空 credential tile，只显示状态和失败信息，不读取密码。
- [ ] 在测试宿主中验证 COM 激活、`GetFieldDescriptorAt`、`GetCredentialCount` 和释放顺序。

### Phase 2：安全 IPC 客户端

- [ ] 实现命名管道连接、超时、长度上限、版本 / request id 校验和错误映射。
- [ ] 接入服务端的一次性认证请求、SID / session 绑定和取消请求。
- [ ] 覆盖服务停止、管道被替换、截断响应、错误 SID、重复 request 和超时。
- [ ] 确认 DLL 不包含 master key、profile 路径读取和服务端监听代码。

### Phase 3：凭据序列化与 stale 状态

- [ ] 在人脸验证成功后获取一次性 `WindowsSecret`，在 `GetSerialization` 内解释 UTF-16LE 并构造系统要求的结构。
- [ ] 使用 Windows 分配器 / `CoTaskMemAlloc` 的正确所有权规则，所有失败分支释放已分配缓冲。
- [ ] 序列化完成后立即清零临时结构、密码 guard 和 IPC 响应。
- [ ] 实现 `ReportResult` 的错误分类、stale 标记、密码过期处理和一次性重试禁止。
- [ ] 覆盖本地账户、Microsoft 账户、错误 SID、密码已修改和离线服务不可用。

### Phase 4：真实 LogonUI 集成

- [ ] 在隔离 Windows 虚拟机中注册测试 CLSID，验证锁屏、解锁、冷启动和注销 / 重启。
- [ ] 验证 Provider 不阻塞 LogonUI：相机 / 识别由认证服务处理，Provider 回调有明确超时。
- [ ] 验证系统密码 tile、辅助功能、取消、切换用户和错误提示仍可用。
- [ ] 验证 TPM2、无 TPM2、服务重启、密钥不可用和 BitLocker 环境下的回退行为。

### Phase 5：安装、升级与切换

- [ ] 增加 Rust DLL、认证服务和符号 / 版本信息的 Windows 打包步骤。
- [ ] 新旧 CLSID 使用独立测试安装；升级时先验证新 Provider，再切换注册项。
- [ ] 安装失败或新 Provider 崩溃时保持旧 Provider / 系统密码登录入口，禁止把系统锁死。
- [ ] 完成服务 ACL、Provider DLL ACL、注册表卸载、重启后残留和回滚测试。

### Phase 6：删除旧实现

- [ ] 新 Provider 通过全部验收后，才把 `packaging/windows/setup.iss` 和 Windows xmake target 切换到 Rust DLL。
- [ ] 更新 `docs/rewrite_master_plan.md`，删除 C++ CP 作为主线的描述。
- [ ] 删除旧 `CredentialProvider/`、旧注册文件和仅供旧 Provider 使用的 IPC 代码；删除动作单独提交。
- [ ] 保留迁移说明和回滚版本，确认干净安装、升级、卸载均可恢复系统密码登录。

## 测试矩阵

### 自动测试

- Rust：COM 对象生命周期、字段索引、协议解析、pipe framing、超时、SID / session 绑定、UTF-16 序列化、zeroization 和 stale 状态。
- `memsafe` fork：Windows page protection、异常展开、锁页失败回滚、并发 guard、stale pointer 和 dump 排除。
- 构建：x64 MSVC 原生构建为发布门槛；MinGW 交叉构建用于开发机回归；`xmake build` 不得修改用户配置文件。
- 静态检查：禁止 `static mut`、`unsafe impl Send/Sync`、密码 `String` / `Vec`、`unwrap()` 出现在 COM / IPC 生产路径。

### Windows 平台验收

- 本地账户：锁屏、冷启动、重启、离线、密码错误、密码修改、账户禁用。
- Microsoft 账户：规范 provider / UPN、网络不可用和密码轮换。
- TPM2：CNG Platform Crypto Provider、重启后解包、TPM 清除 / 主板更换后的 unavailable 和重录流程。
- 无 TPM2：machine DPAPI fallback、BitLocker 建议、不能伪装成 TPM 保护。
- 失败回退：服务不可用、Provider 崩溃、管道异常、密钥损坏时仍能使用 Windows 密码 tile。
- 账户范围：域 / Entra 和 `CPUS_CREDUI` 在专项验收前保持禁用。

## 验收标准

- 新 DLL 由 Windows 原生 LogonUI 正常加载，且不需要 C++ Credential Provider 运行时。
- Provider 不监听 IPC、不打开密文存储、不持有 master key；密码只在一次 serialization 调用期间存在。
- 错误密码最多触发一次自动提交，之后进入 stale 并要求用户重新确认，不造成账户锁定循环。
- TPM2 和无 TPM2 的状态、失败和恢复路径可观测但不泄露密码、embedding 或明文 payload。
- 新实现通过本地 / Microsoft 账户的锁屏、冷启动、离线和密码变更测试，旧系统密码 tile 始终可用。
- 安装升级、卸载和回滚不会留下不可移除的 Credential Provider 注册或无法登录的系统。

## 开放决策

- [待定] 新 Provider 是否使用新的 CLSID 并在灰度验收后切换，还是在最终发布时复用旧 CLSID。
- [待定] `memsafe` Windows `PAGE_NOACCESS` 和无 panic Drop 补丁是维护 fork 还是上游贡献。
- [待定] 是否需要同时发布 MSVC 和 MinGW 构建；MSVC 原生验收不可省略。
- [待定] 域 / Entra 账户和 `CPUS_CREDUI` 的后续支持范围。
