# Smile2Unlock Linux Completion Plan

## Summary

本计划用于把当前已经能够构建和运行的 Linux 实现，推进到可以稳定完成桌面配置、人脸录入、开机登录和锁屏解锁的可交付状态。

当前仓库已经具备 Slint GUI、Rust 配置与档案存储、V4L2 摄像头、SeetaFace 识别与活体检测、PAM 模块、root `su_authd`、Unix control socket、systemd unit 和自动测试。剩余工作的核心不是继续搭建代码骨架，而是完成真实系统认证闭环、处理摄像头和桌面环境集成、补齐 GUI 发行安装，并验证多用户及失败回退。

本计划是 `docs/rewrite_master_plan.md` 中 Linux Phase 3 的后续落地计划。Windows compatibility、可选 SIMD / Zig 扩展和 DMS Monet 配色分别保留在原计划及 `docs/dms_monet_theme_plan.md` 中，不阻塞 Linux 第一版跑通。

## Current Status (2026-07-26)

Phase 1 已完成；Phase 2 和 Phase 3 的实现已完成，真实注销 / 冷启动、DMS 锁屏和摄像头恢复矩阵按当前决定暂缓；Phase 4 打包基础设施及 DEB / RPM 实包验证已完成，源码树内的完整 Release 构建现在还可从 GUI 通过 `pkexec` 安装缺失的系统组件，但真实密码弹窗和安装后状态切换仍待现场验收；Phase 5 多用户隔离和临时账户系统验收已完成，新鲜人脸录入仍待人工验证；Phase 6 的 system-owned 加密存储、fd 安全读取、限流、运行时恢复和 systemd sandbox 已完成；Phase 7 的 UI 重写、国际化、DMS / Monet Phase 1-5、发布级诊断、首次使用流程和键盘可用性均已完成。

- `xmake build` 已通过。
- 当前配置包含 19 个 Xmake target；12 个 Xmake test case 和 42 个 Rust unit test 已通过。
- 临时 PAM 验收入口已覆盖真实 accepted、rejected 和 unavailable 结果，未修改 `/etc/pam.d`。
- accepted 请求已贯通 PAM module、root socket、已安装 daemon、目标用户档案、V4L2、SeetaFace、活体检测和特征比对。
- Release 与已安装的 daemon / PAM module 哈希一致。
- `su_authd` 已重启到带结构化诊断日志的版本，并报告 `available=true`。
- accepted 日志结果为 `face matched`，本次请求耗时 2621 ms。
- `tar.gz` 和 Arch `pkg.tar.zst` 均已生成，包内 GUI / daemon RPATH 已改为包内相对路径。
- 打包 staging 已包含 GUI、PAM、daemon、模型、语言包、Slint / SeetaFace runtime、systemd、桌面入口、图标和许可证。
- `smile2unlock-2.1.3.deb` 和 `smile2unlock-2.1.3.rpm` 已真实生成，并分别在 Debian / Fedora 容器中通过元数据、root 所有权、文件清单、权限、相对 RPATH 和无维护脚本验证。

Phase 2 必须通过真实 display manager 注销和重启完成，不能由进程内 PAM 测试替代。

## Verified Baseline

截至 2026-07-19，仓库和当前开发机已确认：

- Release 配置启用了 Slint、SeetaFace、Zig 和 SIMD 选项。
- `su_app`、`su_authd`、`pam_smile2unlock.so` 和 SeetaFace 模型能够生成。
- Linux GUI 可以访问 V4L2 摄像头，完成人脸预览、录入、删除、认证测试和设置保存。
- `su_authd.service` 已安装、启用并能够创建 `/run/smile2unlock/control.sock`。
- PAM 模块和 daemon 的已安装文件与当前 Release 构建产物一致。
- 当前 greetd PAM 栈已经包含 `pam_smile2unlock.so`，密码认证仍保留为回退路径。
- 用户配置保存在当前用户 XDG config；人脸档案由 daemon 加密保存到 system-owned per-UID store。
- 自动测试已经覆盖 Rust core、control socket、真实 PAM module 加载和 SeetaFace pipeline smoke。

当前尚未形成充分证据证明：安装后的人脸认证已经在真实 greetd 登录、重启后的开机登录及 DMS 锁屏中完整成功。因此，现状定义为“实现和部署基本完成，真实系统验收未完成”。

## Definition Of Done

Linux 第一版跑通需要同时满足：

- 普通用户可以从已安装的桌面入口启动 `su_app`，不依赖源码目录或 `~/.xmake/packages`。
- 用户可以选择摄像头、录入至少一份人脸档案、测试认证并保存活体检测设置。
- 开机后 `su_authd` 在 display manager 认证前可用。
- greetd 登录可以通过有效人脸成功，失败或不可用时可以继续使用密码。
- DMS 锁屏使用明确的 PAM service，并能通过人脸或密码解锁。
- GUI 预览不会长期阻止锁屏或登录认证使用摄像头。
- 两个本地 Linux 用户可以分别管理自己的档案并认证，互不读取对方数据。
- 模型缺失、摄像头不可用、无档案、档案损坏、daemon 停止等情况均安全回退，不造成登录锁死。
- 安装、升级、卸载和 PAM 回滚步骤明确且可复现。

## Non-goals

Linux 第一版不要求：

- 管理员在一个 GUI 中集中维护所有用户的人脸档案。
- 支持 home 在密码登录前不可访问的所有加密 home 方案。
- 在第一版同时发布所有 Linux 发行版的软件包。
- 使用人脸认证自动解锁需要登录密码的桌面 keyring。
- 完成 Windows Credential Provider 重写。
- 完成 PipeWire 摄像头或可选 recognizer 进程拆分。

## Phase 1: Real PAM Acceptance

### Goal

在不影响密码登录和当前会话的前提下，证明 PAM module、control socket、daemon、摄像头、SeetaFace 和用户档案能够在真实认证请求中闭环工作。

### Tasks

- 增加或整理一个专用的 PAM 验收入口，避免第一次验证只能通过注销或重启进行。
- 验证 PAM 请求中的用户名与 daemon 读取的目标用户一致。
- 验证有效人脸返回 `PAM_SUCCESS`。
- 验证无脸、错误人脸、活体检测失败和认证超时返回失败，并继续进入密码认证。
- 验证 daemon 停止、socket 缺失、模型缺失、摄像头不可用、无档案和不安全文件权限均不会绕过认证。
- 保留 PAM 配置备份和已登录 root shell，记录恢复步骤。
- 给 daemon 增加足够的结构化诊断日志，至少区分请求开始、accepted、rejected、busy 和 unavailable；不得记录特征向量或图像。
- 给诊断路径增加服务、模型、摄像头和用户档案可用性检查，减少只能依赖登录失败反推问题的情况。

### Acceptance

- 在当前会话内完成成功、拒绝和服务不可用三类真实 PAM 验证。
- 每种失败都能继续使用原有密码认证。
- 日志能够定位失败阶段，且不包含生物特征或敏感认证数据。

### Result (2026-07-19)

- accepted：通过，有效人脸返回 `PAM_SUCCESS`。
- rejected：通过，不存在的 NSS 用户返回 `PAM_AUTH_ERR`。
- unavailable：通过，不存在的 control socket 返回 `PAM_AUTHINFO_UNAVAIL`。
- daemon 日志：通过，记录 request id、类型、用户、结果、固定原因和耗时。
- 密码回退：模块返回值和现有 PAM 栈顺序已确认；真实 greeter 中的交互回退归入 Phase 2 验收。

## Phase 2: Greetd And Boot Login

### Goal

验证人脸认证在真实 display manager 生命周期和系统重启后可用。

### Tasks

- 确认实际 display manager 使用的 PAM service，而不是根据发行版名称假设。
- 注销到 greeter，分别验证人脸成功和密码回退。
- 重启后确认 `su_authd`、模型、动态库、摄像头设备和 socket 在 greeter 认证前就绪。
- 验证 service 重启后旧 socket 能被安全清理并重新绑定。
- 验证用户未录入档案时不会额外阻塞密码输入。
- 验证纯人脸登录后桌面 keyring 的实际行为，并在 UI 和文档中说明限制。
- 更新 `docs/linux_pam_setup.md` 中的真实验证和回滚步骤。

### Acceptance

- 冷启动后至少连续完成三次人脸登录。
- 人脸失败时密码始终可用。
- 停止 `su_authd` 后仍能使用密码登录。
- systemd 和 greetd 日志中没有动态库、模型路径、权限或 socket 时序错误。

## Phase 3: DMS Lock Screen And Camera Coordination

### Goal

让 DMS 锁屏使用 Smile2Unlock，同时避免桌面 GUI 和系统 daemon 争用 V4L2 摄像头。

### PAM Integration

- 为 DMS 创建独立、可回滚的 PAM service，例如 `dankshell-smile2unlock`。
- 在该 service 中把 `pam_smile2unlock.so` 放在密码模块之前，并保留完整密码回退。
- 通过 DMS 的 `lockPamPath` 选择该 service。
- 不直接修改 DMS 自动生成的用户态 `dankshell` PAM 文件，避免 DMS 同步时覆盖项目配置。
- 分别验证 DMS 锁屏中的成功、拒绝、超时和 daemon 不可用行为。

### Camera Ownership

- 明确 GUI preview、GUI enroll / test 和 daemon authentication 的摄像头所有权规则。
- 会话进入锁定状态时主动停止 GUI preview 并释放 V4L2 fd。
- daemon 打开摄像头遇到 busy 时进行有上限的短暂重试，不突破认证总超时。
- GUI 恢复时不自动抢占正在执行系统认证的摄像头。
- 验证休眠恢复、摄像头热插拔及 `/dev/videoN` 编号变化。

### Acceptance

- GUI 预览开启后触发锁屏，DMS 仍能取得摄像头并完成人脸认证。
- 锁屏认证结束后 GUI 可以由用户重新启动预览。
- 摄像头竞争不会造成 daemon 崩溃、无限等待或密码回退失效。

### PAM Integration Result (2026-07-19)

- 已增加独立的 `dankshell-smile2unlock` PAM 模板，Smile2Unlock 失败后继续系统 `login` 密码栈。
- 已增加不覆盖已有修改、支持回滚的 PAM 安装脚本，并把模板和脚本加入 Linux 包。
- control socket 允许 DMS 用户态 PAM subprocess 连接；daemon 使用 `SO_PEERCRED` 和 NSS 限制普通用户只能认证自己，root 原有控制能力保持不变。
- 普通用户 PAM 验收入口不再依赖 root，并使用私有 XDG runtime 目录。
- DMS v1.5.2 配置校验通过，`lockPamPath` 已部署为 `/etc/pam.d/dankshell-smile2unlock`，IPC 与持久化设置回读一致。
- 已通过普通用户 PAM subprocess 完成本人真实人脸认证，并确认同一用户跨 uid 请求被 daemon 拒绝。
- 真实 DMS 锁屏 UI 中的人脸成功和密码回退仍需在用户可配合锁屏时完成现场验收。
- GUI 通过标准 logind `Lock` 信号停止 preview、取消录入 / 测试认证并释放 V4L2，不依赖 DMS 私有 IPC；解锁后不自动重启 preview。
- daemon 为摄像头释放竞态保留最多 1.2 秒的有界重试；普通认证总时限为 6 秒，启用活体检测时为 12 秒，失败后返回 unavailable 并进入密码回退。
- logind 会话解析和 `Lock` 信号订阅已由独立 Xmake smoke test 覆盖；GUI preview 开启后的真实 DMS 锁屏仍需现场验收。

## Phase 4: Linux GUI Installation And Packaging

### Goal

让 `su_app` 和认证组件可以脱离源码目录安装、升级和卸载。

### Tasks

- 安装 `su_app`、i18n JSON、图标和 `.desktop` 文件。
- 为 GUI 和 daemon 分别定义稳定的安装目录及资源查找规则。
- 移除发布二进制对 `~/.xmake/packages` 的 RPATH 依赖。
- 决定 Slint、SeetaFace 及其 runtime libraries 的打包方式，并明确 libyuv、libjpeg、OpenMP 和系统 GUI 库依赖。
- 让 staged install 覆盖 GUI、daemon、PAM、systemd、模型、语言和图标完整布局。
- 提供安装前检查、升级覆盖、卸载和 PAM 配置恢复路径。
- 第一阶段以 Arch Linux 包或可复现的本机安装布局为基线，再扩展 deb / rpm。
- 增加安装产物的 `ldd` / RPATH 检查，禁止引用构建用户 home。
- 更新 README，使 Linux 构建、运行和安装入口不再隐藏在 Windows 文档之后。

### Acceptance

- 在不包含源码和 xmake package cache 的干净安装环境中启动 GUI。
- GUI 可以找到语言资源、模型和动态库。
- systemd service、PAM module 和 GUI 可以独立升级。
- 卸载后不残留启用的 PAM 引用或失效的 systemd unit。

### Result (2026-07-24)

- DEB / RPM 通过 fpm 使用各自发行版包名声明 libc、C++ runtime、PAM、libyuv、libjpeg、OpenMP 和 systemd runtime 依赖。
- 新增独立包验证脚本，覆盖元数据、root 所有权、安装清单、关键权限、相对 RPATH 和维护脚本缺失检查。
- Debian 与 Fedora 容器验证均通过；宿主机未安装 RPM / fpm 工具。
- 发行包必须在对应发行版家族的最低支持版本构建；在 Arch 上构建后封装为 DEB / RPM 不保证兼容更旧的 glibc。
- 源码树内的完整 Release 构建缺少已安装 helper 时，诊断页会显示“安装部署助手”；点击后通过 `pkexec` 请求管理员密码，运行固定安装脚本，自动探测常见 PAM 模块目录，重载 systemd / D-Bus，并在 GUI 中重新验证 helper。
- 上述 GUI 自举已经通过构建、自动测试和 `DESTDIR` 暂存布局验证；真实 Polkit 密码弹窗和本机系统安装没有作为自动测试执行。

## Phase 5: Multi-user Behavior

### Policy

Linux 端采用“每个用户在自己的桌面会话中管理自己的档案”：

- GUI 默认只读写当前用户的 XDG config 和 data 目录。
- daemon 根据 PAM username 通过 NSS 解析目标 uid 和 home。
- daemon 以目标用户文件权限读取配置和档案，再恢复 root 权限访问摄像头。
- 不提供跨用户人脸档案浏览、复制或集中管理。

### Tasks

- 使用两个本地账户分别录入、认证和删除档案。
- 验证一个用户无法通过路径、符号链接或权限变化影响另一个用户的档案选择。
- GUI 中的用户名显示从可靠的 uid / NSS 来源获取，不只依赖环境变量。
- 明确远程 NSS 用户、无 home 用户和 home 不可访问用户的失败行为。
- 对开机前不可访问的加密 home 做产品决策：第一版明确不支持，或后续设计 system-owned encrypted profile store 和密钥管理。
- 确认用户删除、home 迁移和用户名修改后的清理策略。

### Acceptance

- 两个账户只使用各自档案认证。
- 缺少配置或档案的账户安全回退密码。
- 不支持的 home 类型给出可诊断的 unavailable 结果，不会导致 daemon 或其他用户认证异常。

### Result (2026-07-20)

- 新增独立策略测试，覆盖 root / 本人 / 跨 uid 请求授权、每用户路径隔离、缺失文件、符号链接、错误所有者、目录和组 / 全局可写文件。
- 使用一次性本地账户完成真实 PAM / daemon 系统验收：无档案安全回退、跨用户请求被拒绝，复制为该账户所有的档案后能够进入生物识别判定。
- 符号链接、错误所有者、全局可写、损坏和删除后的 profile，以及不可访问的 home 均返回 unavailable；源用户配置和档案哈希保持不变。
- 系统测试发现并修复了 user file 检查发生在 root filesystem context 的问题；当前路径检查和文件读取均在目标用户 `fsuid` 下执行。
- daemon 系统测试的一次性账户、home 和 runtime 目录已由脚本清理并复核不存在。第二用户的新鲜人脸录入和真实 DMS 锁屏仍需人工验收。
- GUI 用户名改为通过当前 uid 查询 NSS，不再信任可伪造的 `USER` / `LOGNAME` 环境变量。
- 使用第二个一次性账户从打包 runtime 启动 GUI，确认显示该 NSS 用户、识别器 Ready、检测到两路摄像头，并只加载该账户 `0600` XDG 档案；随后通过 GUI 删除档案并确认 profile 数量从 1 变为 0。
- GUI 测试同时修复了安装语言目录解析和不可访问 cwd 导致模型安装目录探测提前失败的问题。测试账户、home、runtime、X11 授权和截图均已清理；新鲜人脸录入仍需镜头前人工配合。

## Phase 6: Security And Reliability Hardening

- 为认证请求增加合理的频率限制和连续失败策略，同时避免与发行版 `pam_faillock` 产生不可预测的双重锁定。
- 按 `docs/credential_storage_encryption_plan.md` 把 profile 迁到 system-owned XChaCha20-Poly1305 store；TPM2 使用 `host+tpm2`，无 TPM2 使用明确标识的 host-key fallback。
- 检查用户路径验证与实际文件打开之间的竞态，优先使用 fd-based 和 `openat2` 风格的安全读取方案。
- daemon 必须继续通过 `SO_PEERCRED` 授权：root 可使用完整控制协议，普通用户只能认证 NSS uid 与 peer uid 一致的本人；继续限制 control frame 大小、协议版本和超时。
- 验证 systemd sandbox 对目标用户 home、摄像头设备和需要的动态库只开放必要访问。
- 评估 SeetaFace 模型懒加载、空闲释放和 daemon 常驻内存占用。
- 增加 camera / model initialization 的恢复能力，避免一次启动失败永久标记服务不可用。
- 明确人脸认证是密码替代还是第二因素；第一版保持密码替代时必须说明 keyring 不会自动获得密码。
- 禁止日志、core dump 和诊断输出包含图像、embedding 或完整 profile JSON。

### Result (2026-07-20)

- daemon 在目标用户 `fsuid` 下用 `openat2` 固定 home 和 user file fd，拒绝路径中任意符号链接、错误 owner、组 / 全局可写文件及超限文件；Rust 只读取 `/proc/self/fd` 指向的已固定对象，原路径替换不会改变本次认证输入。
- 对过滤 `openat2` 并返回 `ENOSYS` 的 service 环境提供逐级 `openat(O_NOFOLLOW)` fd-relative 回退，保持相同的禁止符号链接约束。
- 增加每 uid 一秒的内存态启动间隔；限流返回 busy 并立即进入密码回退，不累计失败次数、不持久化，也不与 `pam_faillock` 形成第二套账户锁定。
- SeetaFace 初始化失败不再永久缓存，每五秒允许惰性重试；实测缺模型返回 unavailable，补齐模型后同一 daemon 无需重启即可恢复到摄像头判定。V4L2 继续在每次认证时重新枚举 / 打开并进行 1.2 秒有界重试。
- systemd unit 限制网络、namespace、内核接口、能力和设备访问，仅保留 `CAP_SETUID`、只读 home、AF_UNIX 和 `video4linux` 字符设备；真实 PAM 请求可加载模型、读取目标用户档案并使用摄像头。`systemd-analyze security` 暴露评分由 8.1 降至 3.1（OK）。

## Phase 7: Linux UX Completion

该阶段不阻塞真实认证闭环，但影响正式发布质量：

- [x] 根据 `docs/dms_monet_theme_plan.md` 接入 DMS / Monet 配色及内置回退主题。
- [x] 完成窗口尺寸、高 DPI、长翻译文本和键盘导航检查；自定义导航与操作按钮支持焦点、Enter 和 Space，窗口默认使用更宽的 `1560x880` 首选尺寸与 `1180x700` 下限。
- [x] 语言包使用外部 JSON；增加语言不需要修改 C++ / Slint 业务逻辑。
- [x] 在 GUI 中区分“桌面测试认证”和“系统 PAM 服务可用”。
- [x] 为摄像头、模型、加密档案、daemon、密钥保护和 PAM 集成显示可执行的诊断状态；服务状态通过真实 control request 获取，不再用 socket 文件存在性代替。
- [x] 为首次使用建立服务与加密存储、摄像头与模型、录入、桌面测试和 PAM 集成的顺序流程。

## Test Matrix

### Automated

- `xmake build`
- 所有 Xmake test target
- Rust core unit tests
- SeetaFace image-to-embedding pipeline smoke
- PAM module accepted / rejected / unavailable integration
- 安装 staging 和发布二进制 RPATH 检查
- 多用户路径、权限、符号链接和损坏档案测试
- camera busy、daemon restart 和 socket replacement 测试

### Manual System Tests

| Scenario | Expected result |
| --- | --- |
| 正确人脸 + 活体通过 | 登录或解锁成功 |
| 错误人脸 | 人脸失败，密码仍可用 |
| 照片或活体失败 | 人脸失败，密码仍可用 |
| 无档案用户 | 快速回退密码 |
| daemon 停止 | 快速回退密码 |
| 模型缺失 | unavailable，密码仍可用 |
| 摄像头被占用 | 有限重试后回退密码 |
| GUI preview 后锁屏 | preview 释放，锁屏认证可用 |
| 冷启动进入 greetd | daemon 已就绪，人脸可用 |
| 第二个本地用户 | 只匹配自己的档案 |
| home 不可访问 | unavailable，密码仍可用 |
| 休眠恢复后锁屏 | 摄像头和认证能够恢复 |

## Recommended Commit Breakdown

实现阶段建议按以下提交拆分：

1. `test: add real Linux PAM acceptance harness`
2. `feat: add authentication service diagnostics`
3. `docs: verify greetd boot login and rollback`
4. `feat: integrate Smile2Unlock with DMS lock PAM`
5. `fix: coordinate camera ownership across session lock`
6. `build: install Linux desktop application and assets`
7. `build: package Linux runtime dependencies`
8. `test: verify per-user Linux authentication data`
9. `security: harden profile loading and auth rate limits`
10. `docs: publish supported Linux installation workflow`

每个认证相关提交必须保持密码回退，并在修改真实 PAM 栈之前先通过专用测试配置验证。

## Immediate Next Task

先现场验证诊断页的 GUI 提权部署闭环：点击安装、完成 Polkit 密码认证、确认 helper 立即可用并继续执行初始化和桌面目标配置。随后优先补齐结构化 PAM fixture / 提交前隔离验证与自动回滚，以及安装升级和卸载生命周期。P2 注销 / 重启、第二账户新鲜人脸录入和休眠 / 热插拔等手工测试仍按当前决定暂缓；这些 Linux 发布缺口处理后再进入 Windows Rust Credential Provider Phase 0。
