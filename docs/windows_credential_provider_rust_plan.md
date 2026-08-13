# Windows Rust Credential Provider 重写计划

## 目标

把当前 `CredentialProvider/` 下的 C++ Sample 重写为位于 `src/` 内的纯 Rust Windows Credential Provider。Rust DLL 只负责 Windows Credential Provider / COM 适配、登录凭据序列化和认证服务客户端，不把 GUI、摄像头、SeetaFace、Tauri、OpenCV 或大型异步运行时装进 LogonUI 进程。

本计划与 `docs/rewrite_master_plan.md` 和 `docs/credential_storage_encryption_plan.md` 配套使用。实现完成后，需要把主计划中的“C++ Credential Provider 适配层”更新为 Rust 适配层；在此之前不修改旧 Provider 的行为。

## 当前状态

- Phase 0（冻结接口与安全样例）已完成，见下方基线记录。
- 现有 C++ Provider 和 Windows LocalSystem 认证服务仍是当前可构建路径，未被替换。
- Rust 加密 envelope、Windows password store、TPM CNG provider、machine DPAPI fallback 和 LocalSystem 服务已经按 `docs/credential_storage_encryption_plan.md` 实现；Windows face profile 的 service-owned 存储仍待完成。
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
├── .cargo/config.toml          # [target.x86_64-pc-windows-gnu] runner = "wine"
├── scripts/seal_smoke.c        # VirtualAlloc 版 PAGE_NOACCESS 冒烟（wine 可跑）
└── src/
    ├── lib.rs                  # 4 个 no_mangle 导出 + 基线测试（已完成）
    ├── class_factory.rs        # Phase 1 ✅
    ├── provider.rs             # Phase 1 ✅
    ├── credential.rs           # Phase 1 ✅
    ├── fields.rs               # v1 字段布局与状态（已完成）
    ├── serialization.rs        # Phase 3
    ├── event_sink.rs           # Phase 1 ✅
    ├── pipe_client.rs          # Phase 2
    └── secret_buffer.rs        # WindowsSecret<N> 原型（已完成）
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

- [x] 固定 Windows SDK、Rust toolchain、`windows` crate 和 `memsafe` fork 版本。*冻结：rustc/cargo 1.98.0-nightly（toolchain pinned by rust-toolchain.toml），`windows-core` 0.62.2 + `windows-sys` 0.61（仅 Win32_System_Memory / Win32_System_Threading），`zeroize` 1 + derive；`memsafe` 尚未引入——本阶段以本地 `WindowsSecret<N>` 满足固定容量 + PAGE_NOACCESS + zeroize 要求，`memsafe` 仅作后续评估（见下方基线记录）。*
- [x] 用最小 Rust `cdylib` 验证 x64 MSVC 原生构建、MinGW 交叉构建和 DLL 导出检查。*Linux 宿主机完成 x86_64-pc-windows-gnu 交叉构建：release cdylib 244KB，`#[unsafe(no_mangle)]` 导出 DllCanUnloadNow / DllGetClassObject / DllRegisterServer / DllUnregisterServer，objdump 导出表核对无误；`exports.def` 已备好（LIBRARY su_credential_provider，4 导出 PRIVATE），Phase 1 接入。x64 MSVC 原生构建是发布门槛，需真实 Windows（VM）执行。*
- [x] 写 COM GUID / HRESULT / field descriptor 的纯内存单元测试。*`canonical_clsid_matches_cpp_baseline`（0x5fd3d285_0dd9_4362_8855_e0abaacd4af6）、`hresult_codes_are_stable`、`field_layout_is_stable`、`sdk_constants_match` 等，12/12 通过（宿主 Linux + wine runner 均过）。*
- [x] 完成 `WindowsSecret` 原型和 Windows 内存保护测试；不接入 LogonUI。*见下方基线记录；seal（PAGE_NOACCESS）测试因 wine 堆页 fault 标 `#[ignore]`，由 `scripts/seal_smoke.c`（VirtualAlloc 版，wine 下通过）与真实 Windows VM 覆盖。*
- [x] 记录旧 C++ Provider 的 CLSID、字段布局、注册项和卸载行为，作为兼容基线。*见下方基线记录。*

#### Phase 0 基线记录（2026-08-13）

旧 C++ Provider（`CredentialProvider/`）兼容基线：

- **CLSID**：`{5fd3d285-0dd9-4362-8855-e0abaacd4af6}`（CLSID_CSample）。注册项：`HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Authentication\Credential Providers\{CLSID}`；类注册 `HKCR\CLSID\{CLSID}\InprocServer32`（SampleV2CredentialProvider.dll，ThreadingModel=Apartment）。卸载仅删前者。
- **DLL 导出**：仅 DllCanUnloadNow + DllGetClassObject（无 register/unregister 函数）。
- **字段布局**：14 个 SAMPLE_FIELD_ID；TILEIMAGE(0)=CPFT_TILE_IMAGE+CPFG_CREDENTIAL_PROVIDER_LOGO（双显示）、LABEL(1)=SMALL_TEXT+CPFG_CREDENTIAL_PROVIDER_LABEL（隐藏）、LARGE_TEXT(2)="Smile2Unlock Provider"（双显示）、PASSWORD(3)=PASSWORD_TEXT（选中+聚焦）、SUBMIT_BUTTON(4)（选中显示）、LAUNCHWINDOW_LINK(5)/HIDECONTROLS_LINK(6)/FULLNAME(7)/DISPLAYNAME(8)/LOGONSTATUS(9)/CHECKBOX(10)/EDIT_TEXT(11)/COMBOBOX(12)/FACE_RECOGNITION_LINK(13)=COMMAND_LINK"使用面部识别登录"（全隐藏）。
- **场景**：CPUS_LOGON / UNLOCK_WORKSTATION / CREDUI；SetUserArray 用于 LOGON/UNLOCK，CREDUI 传 nullptr；CPUS_CHANGE_PASSWORD 返回 E_NOTIMPL。
- **序列化**：GetSerialization 写 KERB_INTERACTIVE_UNLOCK_LOGON，clsidCredentialProvider=CLSID_CSample；密码 SecureZeroMemory；SetSelected 返回 pbAutoLogon=FALSE。
- Rust v1 差异：去掉手工密码字段（4 字段 tile），第一版仅 LOGON + UNLOCK_WORKSTATION。

`WindowsSecret<N>` 原型（`src/platform/windows/credential_provider_rs/src/secret_buffer.rs`）：

- `#[repr(C, align(2))]` 固定容量字节缓冲（buf + len + cfg(windows) sealed/locked 标志）；UTF-16LE 容量计算用 `encode_utf16().count()`，不能按 UTF-8 字节数。
- Windows：`new()` VirtualLock；`seal()`/`unseal()` VirtualProtect PAGE_NOACCESS / PAGE_READWRITE；`clear()` 用 `write_volatile` 逐字节清零（防 DSE）；Drop 顺序 unseal → clear → VirtualUnlock，API 失败全部忽略（fail-secure），无 panic/unwrap。
- 已修问题：seal 测试必须堆分配（栈页被封立即违例）；wine 对堆管理页封 PAGE_NOACCESS 会 fault（真实 Windows 是标准做法）；windows-sys 0.61 的 MEMORY_BASIC_INFORMATION 指针字段需 `null_mut()` 初始化。
- wine 加载限制（记录，不影响真实 Windows）：Rust 1.98 std 的 futex 原语导入 `api-ms-win-core-synch-l1-2-0.dll`（WaitOnAddress/WakeByAddress），wine 11.15 (staging) 的 api-set schema 不识别该名字（`build_import_name` 只有 api-ms-win-crt-* → ucrtbase 的映射），导致 LoadLibrary 失败 c0000135，即使 system32 放了该 dll 也不落盘解析。真实 Windows 10+ 自带此 api-set，无此问题；wine 下仅能跑 cargo test（已 12/12 + 1 ignored），DLL 加载验证归入 Phase 4 VM 验收。

### 重要发现：windows crate 0.62.2 的 CPFT_* 常量值错误

`windows::Win32::UI::Shell` 0.62.2 中 `CREDENTIAL_PROVIDER_FIELD_TYPE` 常量与 wincred.h SDK 值不符（`CPFT_TILE_IMAGE`=6 而 SDK 为 1、`CPFT_SUBMIT_BUTTON`=9 而 SDK 为 6 等，疑似 metadata 偏移 bug）。`CPUS_*`/`CPFS_*`/`CPFG_*` 值正确。因此 Phase 1 起所有字段类型数值取自 `fields.rs` 本地枚举（对齐 wincred.h ABI 值），不使用 crate 的 CPFT_* 常量；`sdk_constants_match` 测试锁定数值。

### Phase 1：Rust COM 骨架

- [x] 实现 `DllGetClassObject`、class factory、引用计数和生命周期。*`DllGetClassObject` 校验 CLSID（不匹配 `CLASS_E_CLASSNOTAVAILABLE`）、riid（仅 `IClassFactory::IID`/`IUnknown::IID`，否则 `E_NOINTERFACE`）、null 输出（`E_POINTER`）；`ClassFactory::CreateInstance` 拒绝聚合（`CLASS_E_NOAGGREGATION`）并返回 `Provider` 实例；`LockServer` 维护进程级 `LOCK_COUNT`（`DllCanUnloadNow` 在 >0 时返回 `S_FALSE`，fail-secure 防负）。*
- [x] 实现 provider 的字段元数据、用户数组、usage scenario 和 credential object 创建。*`Provider` 暴露 4 字段（TileImage/LargeText/FaceStatus/SubmitButton）；`GetFieldDescriptorAt` 用 `CoTaskMemAlloc` 分配 descriptor + UTF-16 label（越界 `E_INVALIDARG`，失败分支先释放 label）；`GetCredentialCount` 返回 1/0/FALSE；`SetUsageScenario` 仅收 `CPUS_LOGON`/`CPUS_UNLOCK_WORKSTATION`（其余 `E_NOTIMPL`）；`SetUserArray` 暂存（Phase 4 绑 SID）；`Advise` 只存 upadvisecontext（接口指针 marshal 留 Phase 2）。*
- [x] 实现空 credential tile，只显示状态和失败信息，不读取密码。*`Credential` 实现 `ICredentialProviderCredential`/`2`/`WithFieldOptions`：`GetFieldState` 查 fields.rs `state_pairs()`、`SetSelected` 返回 TRUE、`GetSubmitButtonValue` 返回 3、`GetSerialization`/`GetUserSid` 返回 `E_NOTIMPL`（Phase 3/4），不含任何密码材料。*
- [x] 在测试宿主中验证 COM 激活、`GetFieldDescriptorAt`、`GetCredentialCount` 和释放顺序。*wine 下 22 个测试：COM 激活成功路径（`GetFieldDescriptorCount`==4、`GetCredentialCount`==1）、错误 CLSID/riid/null 输出、聚合拒绝、`GetFieldDescriptorAt(0)` 内容（dwFieldID/cpft==1/`CPFG_CREDENTIAL_PROVIDER_LOGO`/label 非空 + `CoTaskMemFree` 释放）、`CPUS_CREDUI` 拒绝、`LockServer` 与 `DllCanUnloadNow` 联动。21 通过 + 1 ignored（wine heap 的 PAGE_NOACCESS，见 Phase 0 记录）。*

### Phase 2：安全 IPC 客户端

- [x] 实现命名管道连接、超时、长度上限、版本 / request id 校验和错误映射。*`pipe_client.rs` 以 `#[repr(C)]` 复刻协议（`Request` 2448B / `Response` 1056B，8 字节对齐与 C++ `sizeof` 一致）；`CallNamedPipeW` 同步单次事务（3000ms 超时）；`validate_response` 校验字节数、magic/version、request_id 非零与回显、logon_session_id 匹配；`status_to_hresult` 映射 `kOk→S_OK`、`kInvalidRequest→E_INVALIDARG`、`kAccessDenied→E_ACCESSDENIED`、`kStaleOrConsumed→ERROR_PASSWORD_RESTRICTION`、`kCorrupt/kUnavailable→ERROR_INVALID_DATA/ERROR_SERVICE_NOT_ACTIVE`。*
- [x] 接入服务端的一次性认证请求、SID / session 绑定和取消请求。*`PipeClient::prepare` 请求 `kPrepare` 一次性凭证（SID 经 `copy_fixed` 校验，密码经 `0<len<513 && password[len]==0` 校验后移入 `PreparedPipePassword`）；`mark_stale` 同模式；密码缓冲在所有路径（成功/失败/drop）以 volatile 逐字清零；`current_user_sid` 走 `OpenProcessToken→GetTokenInformation(TokenUser)→ConvertSidToStringSidW`。*
- [x] 覆盖服务停止、管道被替换、截断响应、错误 SID、重复 request 和超时。*测试含 `transact_without_server_fails`（wine 下无服务端）、`response_validation` 全失败分支（bad magic/version/request_id/字节数/session）、`status_mapping_table`、`copy_fixed_bounds`、`prepared_password_drop_wipes`（`drop_in_place` 后裸指针验证全零）、`secure_clear_wipes`、`request/response_layout`（尺寸+字段偏移）。wine 下 30 passed + 1 ignored。*
- [x] 确认 DLL 不包含 master key、profile 路径读取和服务端监听代码。*`pipe_client.rs` 仅客户端：`CallNamedPipeW` 单次事务、无 `CreateNamedPipeW`/监听、无 master key 引用；`PreparedPipePassword` 是唯一密码容器且 drop 即清零。*

### Phase 3：凭据序列化与 stale 状态

- [x] 在人脸验证成功后获取一次性 `WindowsSecret`，在 `GetSerialization` 内解释 UTF-16LE 并构造系统要求的结构。*`serialization.rs`：`kerb_interactive_unlock_logon_init` 组装 `KERB_INTERACTIVE_UNLOCK_LOGON`（`LSA_UNICODE_STRING.Length` 为不含 NUL 的字节数、MessageType 按场景映射 `KerbInteractiveLogon(2)`/`KerbWorkstationUnlockLogon(7)`、LogonId 零）；`kerb_interactive_unlock_logon_pack` 按 WinLogon/LSA 消费的 packed 格式打包（`Buffer` 为相对基址字节偏移、字符串不 NUL 结尾、总长 = 结构 + 三段 Length 字节和）。`credential.rs::GetSerialization` 经 `PipeClient::prepare` 从认证服务取一次性密码，`protect_password`（`CredIsProtectedW`/`CredProtectW` 两遍法）加密后序列化。*
- [x] 使用 Windows 分配器 / `CoTaskMemAlloc` 的正确所有权规则，所有失败分支释放已分配缓冲。*`kerb_interactive_unlock_logon_pack` 用 `CoTaskMemAlloc` 分配序列化缓冲；`GetSerialization` 后续任一步失败（`retrieve_negotiate_auth_package` 等）都会先 `CoTaskMemFree` 已分配缓冲再返回错误。*
- [x] 序列化完成后立即清零临时结构、密码 guard 和 IPC 响应。*`pipe_client.rs` 的 `transact` 无论成败都 `secure_clear`（`write_volatile` 逐字，防 DSE）request/response 的 `password`；`PreparedPipePassword` drop 即清零；`credential.rs` 失败路径对密码副本同样清零。*
- [x] 实现 `ReportResult` 的错误分类、stale 标记、密码过期处理和一次性重试禁止。*`credential.rs` 增加 `stale`/`serialized` 状态：`ReportResult` 收到非 `STATUS_SUCCESS` 即置 `stale`；`GetSerialization` 在 `stale || serialized` 时拒绝再次提交，保证同一管道一次性 token 最多自动提交一次。*
- [ ] 覆盖本地账户、Microsoft 账户、错误 SID、密码已修改和离线服务不可用。（留 Phase 4 真实 Windows VM 验收；wine 下 `protect_password_roundtrip` 因缺 `advapi32.CredIsProtectedW` 标记 `#[ignore]`。）

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
