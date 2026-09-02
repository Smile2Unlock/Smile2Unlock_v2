# Smile2Unlock Credential Storage Encryption Plan

## Summary

Smile2Unlock 不再把人脸 embedding、标签或 Windows 登录密码作为明文用户文件保存。Linux 和 Windows 共用 Rust 定义的版本化 AEAD 数据格式，但分别使用平台密钥提供者。高敏感数据由开机前可用的系统认证服务管理，GUI 不直接持有主密钥或打开凭据文件。

当前 profile store 只有约 18 KiB / 用户，一份 SeetaFace embedding 为 1024 个浮点数；认证时仍需扫描全部 embedding。数据库索引不能加速该匹配过程，因此第一版不引入 SQLite 或 SQLCipher。存储采用原子替换的加密文件；未来出现复杂查询需求时，可以在不改变 IPC 和密钥模型的前提下重新评估 SQLCipher。

## Security Goals

- profile embedding、标签、时间和 Windows 密码在磁盘上均为认证加密后的密文。
- 密文被篡改、截断、替换到其他 uid / SID 或使用错误密钥时必须失败关闭。
- Linux 在 display manager 之前、Windows 在 LogonUI / Credential Provider 阶段可以解密所需凭据。
- GUI 只能通过经过 peer identity 校验的系统服务管理当前账户数据。
- 密钥、密码、embedding 和完整明文 payload 不进入日志、环境变量、core dump 或错误文本。
- 支持格式版本、密钥版本、原子迁移和密钥轮换。

## Threat Model

需要防护：

- 其他普通本地用户读取 profile 或 Windows 凭据。
- profile 文件、备份或单独分区副本泄露。
- 密文被替换、截断或复制给另一个账户。
- GUI、安装器或日志意外泄露明文。

明确无法完全防护：

- 已控制 root / LocalSystem 且能读取认证服务内存的攻击者。
- 无 TPM、无全盘加密机器上的完整离线磁盘复制；无人值守开机认证要求机器本身能取得解密密钥，两者无法同时严格满足。
- 第一版不能识别同一账户的一份旧但有效的完整密文；AEAD 能验证完整性和账户绑定，但不能单独提供防回滚计数器。
- 已经进入 Btrfs / SSD 快照、备份或同步历史的旧版明文 JSON。删除原文件不等于安全擦除。

## Storage Decision

### Encrypted Profile Store

建议路径：

- Linux: `/var/lib/smile2unlock/users/<uid>/profiles.s2u`
- Windows: `%ProgramData%\\Smile2Unlock\\Users\\<SID>\\profiles.s2u`

文件归系统认证服务所有。第一版继续把现有 `ProfileStore` JSON 序列化结果作为内部 payload，再使用 `XChaCha20-Poly1305` 整体加密。JSON 不再落盘，因此没有必要为了体积改成 SQLite；后续可通过 payload format 字段迁移到 MessagePack 等二进制编码。

建议 envelope：

```text
magic = "S2UE"
envelope_version
payload_format
payload_version
key_version
account_kind + uid_or_sid_hash
random_salt[16]
random_nonce[24]
ciphertext_length
ciphertext_and_poly1305_tag
```

除密文外的全部 header 作为 AEAD associated data。每次写入使用新的随机 salt 和 nonce，并通过现有临时文件、`fsync`、原子替换流程提交。

每账户数据密钥使用 HKDF-SHA256 派生：随机 `salt` 作为 HKDF salt，master key 作为 input keying material；HKDF info 使用带长度的规范编码，包含 envelope version、account kind、完整 uid / SID、`data_kind` 和 key version。`data_kind` 区分 profile 和 Windows password，避免拼接歧义、跨账户或跨用途密钥复用。header 只保存规范账户标识的 SHA-256，并作为 AAD 验证。

### Windows Password Store

Windows 登录密码必须与 profile 分离：

```text
%ProgramData%\\Smile2Unlock\\Users\\<SID>\\logon-secret.s2u
```

- 只在用户明确启用 Windows 人脸登录并重新输入当前密码后保存。
- payload 包含 SID、由 Windows 账户 API 解析的规范登录名、账户类型、未经 Unicode 规范化的密码 UTF-16LE 字节、创建时间和 credential generation；不得包含可打印诊断副本。规范登录名同样只存在于密文内，不能信任 GUI 自报的账户类型或 SID。
- Credential Provider 不提供通用 `get_password` IPC。人脸验证成功后，它只能用绑定 request id、LogonUI session 和账户 SID 的一次性请求取得登录 credential serialization 所需秘密。
- 密码在受控缓冲区中短暂存在，使用后调用 `SecureZeroMemory`；不得进入 `std::string`、异常文本、日志或 crash dump。
- Credential Provider 的 `ReportResult` 收到密码错误、密码过期或必须修改密码等 credential rejection 后，将该 secret 标记为 stale，停止自动提交，要求用户完成系统密码登录 / 更新后在 GUI 中重新确认。这样不会用旧密码反复触发 Windows 账户锁定。
- Windows Hello PIN 不是账户密码，不得作为替代内容保存。
- 第一版必须验收本地账户和现有 README 承诺的 Microsoft 账户；Microsoft 账户使用 Windows API 返回的规范 provider / UPN 形式，不能靠字符串猜测邮箱格式。Active Directory / Entra 域账户在密码轮换、离线缓存和网络不可用场景完成独立验收前保持禁用。
- secret 只用于 `CPUS_LOGON` 和 `CPUS_UNLOCK_WORKSTATION` 的一次性 serialization，不用于密码修改；是否支持 `CPUS_CREDUI` 必须单独做权限与账户选择威胁建模，第一版默认禁用。

长期目标仍是使用不需要保存可逆密码的 Windows Hello / 系统密钥认证路径；在 Credential Provider 必须提交密码的实现中，上述方案只把风险压到平台允许的最低范围，不能声称 LocalSystem compromise 下密码仍安全。

## Key Providers

上层 Rust storage 只依赖 `KeyProvider` 返回 32 字节 master key 和 key version，不感知 TPM、systemd 或 CNG。

首次初始化时选择 provider，并把 provider id 和 key version 写入不含秘密的系统元数据。之后 TPM 暂时不可用、被清除或 provider 出错都必须返回 `unavailable`，不能现场生成 host / DPAPI key 或静默降级；从硬件保护迁移到软件回退必须是显式管理操作，并且只有在旧 key 仍可解密时才能重加密。

### Linux With TPM2

当前开发机 `systemd-analyze has-tpm2` 返回 `yes`。安装时：

1. 使用内核 CSPRNG 生成随机 master key，禁止从 machine-id、用户名或密码派生。
2. 使用 `systemd-creds encrypt --with-key=host+tpm2` 生成 encrypted credential。
3. unit 使用 `LoadCredentialEncrypted=smile2unlock-master.key:...`。
4. daemon 只从 `$CREDENTIALS_DIRECTORY/smile2unlock-master.key` 读取，读取后锁定 / 清零内存，不通过环境变量传递。

`host+tpm2` 同时绑定 TPM 和本机 host secret。PCR / Secure Boot 策略必须经过内核、固件和 Secure Boot 更新测试后再收紧，避免正常升级导致所有用户必须重新录入。

### Linux Without TPM2

安装器检测不到可用 TPM2 时，使用：

```text
systemd-creds setup
systemd-creds encrypt --with-key=host ...
```

host secret 位于 `/var/lib/systemd/credential.secret`，由 root 权限保护。service 仍通过相同的 `LoadCredentialEncrypted=` 接口读取，因此业务代码和文件格式不分叉。

该回退能防普通用户和单独 profile 文件泄露，但不能防攻击者同时复制系统盘和 host secret。无 TPM 机器若要求离线防护，必须启用 LUKS / 全盘加密，或由管理员提供启动时外部密钥；使用用户密码派生 key 会破坏无人值守人脸开机登录，因此不作为自动回退。

诊断界面必须显示 `TPM2-bound`、`host-key` 或 `unavailable`，不得把 host-key 描述为硬件保护。创建不了任何安全 key provider 时，系统认证保持禁用，不能退回明文。

### Windows With TPM

- 使用 Windows CNG `Microsoft Platform Crypto Provider` 和 `NCRYPT_MACHINE_KEY_FLAG` 创建 machine-scoped、non-exportable wrapping key，并把 key ACL 限制到认证服务身份。
- 随机 master key 只以 TPM key 包装后的形式持久化；Windows 认证服务以 LocalSystem 身份在开机阶段解包。
- profile 与 password 仍由 Rust AEAD 层加密，CNG 只负责保护 master key，避免维护两套数据格式。

### Windows Without TPM

- 使用 `CryptProtectData(..., CRYPTPROTECT_LOCAL_MACHINE, ...)` 封装随机 master key，并让认证服务创建仅 `SYSTEM` 可读的包装文件；管理操作通过受限服务接口完成，不给普通桌面进程文件读取权限。
- 可评估 `Microsoft Software Key Storage Provider` 的 non-exportable machine key，但第一版只保留一个明确 fallback，避免双重封装逻辑。
- 不使用用户级 DPAPI、Credential Manager 或 Windows Hello PIN：它们在用户登录前不可可靠取得，或不等同于账户密码。
- machine DPAPI fallback 不能抵御获得完整系统盘和 Windows machine secret 的离线攻击；无 TPM 时应建议 BitLocker。

## Service And IPC Changes

Linux `su_authd` 和未来 Windows LocalSystem auth service 成为唯一 storage owner。control protocol 增加：

- `storage_status`
- `list_profiles`
- `enroll_profile`
- `delete_profile`
- `migrate_profile_store`
- Windows-only `store_logon_secret`、`clear_logon_secret`、`prepare_logon_credential`

规则：

- Linux 使用 `SO_PEERCRED`，Windows 使用命名管道 ACL、client token / SID impersonation 和一次性 session token。
- 普通用户只能管理与 peer uid / SID 相同的账户。
- embedding 可以由 GUI 提取后通过本地认证通道发送；原始图像不得进入 storage IPC。
- 返回 profile 列表时只返回 UI 所需 id、label 和时间，不返回 embedding。
- Windows 密码不得通过 list/status/diagnostic API 返回。

## Migration

1. daemon 检测安全的旧版 `profiles.json`，但不会在日常认证中长期保留明文 fallback。
2. 用户在已登录 GUI 中确认迁移；GUI 通过 authenticated IPC 请求 daemon 导入。
3. daemon 在目标 uid 权限下固定并读取旧文件，验证 schema 后写入 system-owned encrypted store，再读回并完成一次匹配自检。
4. 成功后移除旧路径、`fsync` 父目录并记录不含生物数据的 migration marker；失败时保留原文件且不切换 active store。
5. 发布说明明确旧文件可能仍存在于快照和备份中。需要强保证时，应清理相关快照 / 备份或重新录入。
6. TPM 清除、主板更换或 Windows machine key 丢失时，默认策略是清除不可解密 store 并重新录入；可选恢复包必须由管理员使用独立口令离线保存，不能在本机放置等价明文 recovery key。

## Tests

- 固定向量验证 XChaCha20-Poly1305、HKDF、header 和 AAD。
- 错误 key、uid / SID、data kind、nonce、header、tag、截断和超大输入全部失败。
- 同一 payload 重写产生不同 ciphertext。
- key version 轮换和旧 payload version 迁移。
- 原子写中断不会破坏上一份有效 store。
- TPM2 credential、host credential 和 Windows DPAPI machine fallback 的平台集成测试。
- daemon 重启、无 key、key 损坏、TPM reset 模拟和权限错误时安全失败，并保留系统密码登录入口；不得把已选 TPM provider 静默改成软件 provider。
- Windows stale password 只提交一次并禁用，不触发重复账户锁定。
- Windows 本地账户和 Microsoft 账户分别覆盖锁屏、冷启动、网络不可用、密码已修改和错误账户 SID；域账户及 `CPUS_CREDUI` 在未通过专项验收时不会暴露自动提交入口。
- 测试和日志扫描确保不输出 password、embedding 或明文 payload。

## Implementation Order

1. 在 Rust 中实现版本化 envelope、AEAD、HKDF、zeroize 和纯内存测试。
2. 实现 Linux `KeyProvider`：`host+tpm2`、`host` fallback 和 storage diagnostics。
3. 把 profile 写入迁到 system store，并扩展 daemon / GUI IPC。
4. 实现旧 JSON 一次性迁移，移除日常明文读取路径。
5. 实现 Windows CNG TPM provider 和 DPAPI machine fallback。
6. 实现独立 Windows password envelope、stale 检测及 Credential Provider 一次性提交。
7. 完成 key rotation、恢复 / 重录流程、打包和跨发行版测试。

## Acceptance Criteria

- 对 profile / password 文件运行 `strings`、JSON parser 或 SQLite 工具不能恢复标签、embedding 或密码。
- 篡改任何受保护字段都会导致认证 unavailable，不会使用部分数据继续认证。
- TPM2 机器显示硬件绑定并能在正常重启后登录；无 TPM 机器明确显示 host-key fallback。
- 删除 encrypted credential 或清除 TPM 后仍保留密码登录回退，不会锁死系统。
- Windows 密码错误后不重复自动提交，用户可以回到系统密码 tile。
- 本地账户和 Microsoft 账户能以系统解析的规范身份完成登录 / 解锁；未验收的域账户和 `CPUS_CREDUI` 不读取或提交已保存密码。
- GUI 和 PAM / Credential Provider 不直接访问 master key 文件。

## Implementation Status (2026-07-26)

Completed in source and automated tests:

- Rust XChaCha20-Poly1305 envelope, HKDF account/data-kind separation,
  versioned payloads, zeroization, tamper rejection and Windows password stale
  state.
- Linux systemd credential key provider with TPM2-bound and host-key modes,
  root-owned per-UID profile stores, daemon-only profile management and explicit
  legacy JSON migration.
- Linux GUI/PAM IPC migration, storage diagnostics, packaging, systemd unit and
  tarball staging.
- Windows CNG TPM wrapping with machine-DPAPI fallback only when the platform
  provider is unavailable, SYSTEM-only ACL enforcement and a LocalSystem SCM
  service.
- Windows current-user store/clear pipe operations, LocalSystem-only one-time
  prepare/stale operations and Credential Provider LOGON/UNLOCK serialization.
- Removal of the legacy SQLite/AES-CBC and UDP password-return path. Existing
  SQLite password fields are retired without decryption and require the user to
  enter the current Windows password again.
- MinGW builds for `Smile2UnlockAuthService.exe` and the current C++ Credential
  Provider DLL, including a Windows Rust static library. Linux `xmake build`,
  all 12 Xmake tests and the release tarball staging pass. The planned pure
  Rust Credential Provider remains a separate, not-yet-started rewrite.

Still requires platform acceptance before release:

- Run the service on physical Windows TPM and non-TPM machines and verify CNG,
  machine DPAPI, ACLs, service restart and installer upgrade/uninstall behavior.
- Exercise local and Microsoft accounts through cold boot, lock/unlock, offline
  login, wrong/stale password and password-change flows. Domain/Entra accounts
  remain disabled.
- Add Windows system-service ownership of encrypted face profiles; the current
  completed profile daemon/storage integration is Linux-only.
- Implement an administrator-driven key rotation/re-encryption command and the
  documented clear-and-re-enroll recovery workflow for a lost TPM/machine key.
- Validate the PowerShell staging script and `setup.iss` with native Windows
  tooling; the Linux development host does not provide PowerShell or Inno Setup.
