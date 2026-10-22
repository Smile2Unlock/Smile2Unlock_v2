# mcpp 可选构建与编译速度评估

mcpp 仅用于可移植 C++ 核心的附加构建。应用程序、Rust、界面、安装包和 CI
继续使用 xmake。这里固定 mcpp 2026.10.21.1；默认选择其管理的 GCC 16.1.0，
不修改原有 `xmake.lua`，也不移动现有源码。

**构建与测试。** 在仓库根目录执行：

```bash
mcpp toolchain install gcc@16.1.0
mcpp build --workspace --jobs "$(nproc)"
MCPP_JOBS="$(nproc)" mcpp test --workspace --strict
MCPP_JOBS="$(nproc)" mcpp test --workspace --profile release --strict
```

此版本 `mcpp test` 不接受 `--jobs`，需要使用 `MCPP_JOBS`。`build` 不编译测试；
`test --no-run` 编译测试但不执行，适合测量编译时间。只构建日志/诊断核心可以使用
`mcpp build -p log -p diagnostics`。`mcpp.lock` 记录 JSON 3.12.0 的解析结果；
该版本 mcpp 的锁文件本身尚不保证冻结未来解析，所以清单也写明精确依赖版本。

| 部分 | 清单/源文件范围 | 测试 |
| --- | --- | --- |
| 日志 | `src/common/log/mcpp.toml`；core、sinks、logger、process；Linux 的 file_posix 或 Windows 的 file_windows、windows_directory | log/core_test、log/sinks_test |
| 诊断核心 | `src/common/diagnostics/mcpp.toml`；仅 core.cpp | diagnostics/core_test，35 项检查 |
| 控制 socket | `src/modules/mcpp.toml`；仅 Linux 的 su.control.socket.cppm | socket_smoke、socket_deadline |

根清单承载 socket 测试，`tests/mcpp.toml` 承载其余三个测试。这是因为 mcpp 拒绝
发现包目录之外的测试文件；源码包的 `[test] discover = []` 防止自动带入其他测试。
测试 glob 从通配目录开始，保留 log/ 与 diagnostics/ 前缀，避免两个 core_test 重名。
日志 golden 文件继续使用 `tests/log/format_golden.txt`。测试在未定义 `SU_LOG_GOLDEN`
时从 `__FILE__` 定位同目录 fixture，适应 mcpp 的运行目录；xmake 原来的绝对路径
宏保持有效。产品 C++ 源码没有改动。

注册表的 `nlohmann.json` 3.12.0 确实导出 `single_include/nlohmann/json.hpp`，
因此现有 `#include` 可直接使用。它还生成并编译 `nlohmann.json` 模块，即使本项目
并没有 import 这个 JSON 模块；其成本纳入冷缓存测量。

**使用现有 GCC。** mcpp 支持 path 工具链，不局限于其管理的编译器。在根清单
追加以下配置后，Linux 使用与 xmake 相同的 `/usr/bin/g++`：

```toml
[workspace.toolchain.linux]
path = "/usr"
family = "gcc"
```

```bash
export PATH="$HOME/.mcpp/registry/data/xpkgs/xim-x-ninja/1.12.1:$PATH"
MCPP_JOBS="$(nproc)" mcpp test --workspace --strict
```

需要支持 C++26 和 `import std` 的 GCC/标准库；本机为 GCC 16.2.1 20260810。
必须写 `family = "gcc"`，因为 `/usr/bin` 同时存在 Clang，单独的
`MCPP_TOOLCHAIN=path:/usr` 可能选到 Clang。path 配置不会自动把 mcpp 的 ninja 加入
PATH，上面的路径是本次环境实际安装位置；其他版本应替换版本目录。
默认受管理工具链不需要这一步。不要设置 `sysroot = "/"` 来绕过运行库约束。

**Windows。** 在 Linux 上只选择 portable-tests，避免选择 POSIX socket：

```bash
mcpp toolchain install gcc@16.1.0 --target x86_64-windows-gnu
MCPP_JOBS="$(nproc)" mcpp test -p portable-tests \
  --target x86_64-windows-gnu --no-run --strict
```

此次受管理 MinGW 16.1.0 下载约 103.5 MB，安装时间未计入基准；日志、诊断核心和三个
测试均交叉编译成功。三个 exe 手动在 Wine 下分别退出 0，包含 Windows 日志
多进程写入与轮转测试。这是 Wine 验证，不代表真实 Windows 登录/安全桌面验收。
从仓库根目录运行 exe 的命令形式为：

```bash
WINEDEBUG=-all wine target/x86_64-windows-gnu/<fingerprint>/bin/portable-tests/log/core_test.exe
WINEDEBUG=-all wine target/x86_64-windows-gnu/<fingerprint>/bin/portable-tests/log/sinks_test.exe
WINEDEBUG=-all wine target/x86_64-windows-gnu/<fingerprint>/bin/portable-tests/diagnostics/core_test.exe
```

也试过 `[workspace.toolchain.windows] path="/usr"`、
`prefix="x86_64-w64-mingw32-"`、`family="gcc"`，使用本机 MinGW 16.2.0。
当前 mcpp 在编译前拒绝该组合：Windows target 的 C library 必须来自 gcc@16.1.0
payload，而 path:/usr 没有该提供者。PE 没有 PT_INTERP 并不能绕过这一前置检查。
因此没有声称系统 MinGW 构建成功，也没有将此机器路径写入默认清单。

**未纳入的部分。** 下面的边界结合评审者的独立探测和此次实际构建结果，
不把“当前默认沙箱看不到头文件”误写成工具永远无法使用系统库。

| 部分 | 具体原因与后续工作 |
| --- | --- |
| PAM 模块 | 默认沙箱找不到 security/pam_appl.h。评审者确认受管理工具链配合 `-idirafter /usr/include`、`allow_host_libs = true` 和 `-lpam` 可以编译。PAM 是由宿主进程加载的共享库，不受 exe 的 PT_INTERP 影响；在宿主 glibc 2.44 与沙箱 2.44.3 兼容的本机属于“可能可行，未继续验证”。此次不添加 PAM 源码或宿主 include 绕行配置。 |
| sd-bus、su_authd、部署助手 | 评审者确认相同机制加 `-lsystemd` 可编译，但 exe 的 PT_INTERP 指向 mcpp 私有 glibc loader，该 loader 默认找不到 /usr/lib 的宿主共享库；需要显式运行库闭包、部署与发行版兼容方案，不能直接作为发行版安装包交付。仍使用 xmake。 |
| path 工具链的宿主 sysroot | `sysroot="/"` 可以编译宿主头文件，但 RuntimeBinding 仍指向私有 glibc@2.44.3，运行库闭包校验以“one process cannot have two libcs”拒绝；`allow_host_libs` 不解除该冲突。 |
| libyuv、Slint、SeetaFace | 当前注册表没有 libyuv/Slint 包；SeetaFace 是本项目的本地预编译 SDK，尚未描述其平台、ABI、头文件与运行库闭包。libpng/libjpeg-turbo 已有包不等于整条图像链可直接迁移。 |
| Rust 与 Slint 代码生成 | 需要 build.mcpp 或规则包显式建模 Cargo、生成头文件、平台 ABI、依赖与运行库；此次不触及 Rust crate、Slint 或构建规则。 |
| 部署、打包、CI | 包含平台集成、签名、资源和发行版运行库要求，继续使用现有 xmake 流程。此次不修改部署辅助程序、打包或 CI。 |

**基准方法。** 2026-10-22，本机 Intel Core i7-13650HX，`nproc=20`。
xmake 3.1.2+20261016，mcpp 2026.10.21.1。主比较双方都使用宿主 GCC 16.2.1；
另列受管理 GCC 16.1.0，不能把它的差异全部归因于构建工具。

```bash
python -m unittest discover -s tests/tooling -p test_mcpp_benchmark.py -v
python scripts/benchmark-mcpp.py --runs 3 --output build/mcpp-benchmark
```

脚本会临时追加宿主工具链配置，结束或异常时恢复根清单、原有 `.xmake` 和源文件
mtime。受影响的单个 std 全局缓存条目也会先保存并在测量后恢复；不会清空机器上
其他项目的缓存。不要与其他构建并发运行脚本。详细命令、退出状态、逐次时钟读数、
日志、编译数据库和原始采样数据都保存在输出目录（默认 `build/mcpp-benchmark`，不纳入版本控制）。

xmake 使用现有七个目标 `su_log su_diagnostics su_log_test su_log_sinks_test
su_diagnostics_test su_control_socket_smoke_test su_control_socket_deadline_test`，
通过临时 `.mcpp-benchmark.lua` 导入原清单并移除 su_diagnostics 的 export.cpp、
process_posix.cpp、export_posix.cpp；原 `xmake.lua` 不变。配置命令为：

```bash
xmake f -F .mcpp-benchmark.lua -c -y -p linux -a x86_64 -m debug \
  --with_slint=n --with_seetaface=n --ccache=n \
  --builddir=build/mcpp-benchmark-xmake/debug
xmake build -F .mcpp-benchmark.lua -j 20 su_log su_diagnostics \
  su_log_test su_log_sinks_test su_diagnostics_test \
  su_control_socket_smoke_test su_control_socket_deadline_test
MCPP_JOBS=20 MCPP_OFFLINE=1 mcpp test --workspace --no-run --profile dev --cache global
```

release 对应 xmake 的 `-m release` 和 mcpp 的 `--profile release`。
每个场景测量三个无休眠的样本，表中为中位秒数；下载、工具链安装、包解析/安装配置、
执行测试不计入。冷构建逐次删除对应构建目录；xmake 关闭编译缓存。
mcpp“全局冷”使用 `--cache off` 并暂时移开当前 std 缓存，因为该选项本身仍会读取
全局 std；“全局热”保留 JSON 和 std 全局缓存，但同样逐次删除项目构建目录。
增量场景分别只 touch core.h 和 su.control.socket.cppm，不改文件内容。

机器休眠后，先前所有计时已废弃，完整重跑。每条命令前后分别记录
`time.clock_gettime(CLOCK_MONOTONIC)` 和 `time.clock_gettime(CLOCK_BOOTTIME)`；
两个耗时差的绝对值超过 1 秒即输出 DISCARD，保存拒绝记录，重新执行该场景的
清理/touch 准备后重试。中位数只使用通过检查的 MONOTONIC 耗时；
同时保留 `date +%s.%N` 耗时作为交叉检查。模拟测试覆盖休眠丢弃与重试、重复准备、
真实构建失败不被重试掩盖、1 秒阈值边界。

| 模式 | 场景 | xmake / GCC 16.2.1 | mcpp / 同一 GCC 16.2.1（主比较） | mcpp / GCC 16.1.0 |
| --- | --- | ---: | ---: | ---: |
| debug / dev | 冷构建：全局冷 | 8.606 | 8.672 | 8.829 |
| debug / dev | 冷构建：mcpp 全局热 | — | 5.543 | 6.148 |
| debug / dev | 无改动重建 | 0.196 | 0.104 | 0.652 |
| debug / dev | touch 日志 core.h | 1.854 | 2.665 | 3.220 |
| debug / dev | touch socket 模块 | 5.209 | 5.387 | 4.537 |
| release | 冷构建：全局冷 | 12.322 | 11.370 | 12.505 |
| release | 冷构建：mcpp 全局热 | — | 8.881 | 8.984 |
| release | 无改动重建 | 0.200 | 0.103 | 0.649 |
| release | touch 日志 core.h | 1.932 | 3.181 | 3.689 |
| release | touch socket 模块 | 8.846 | 8.224 | 5.974 |

std 的一次性编译未被 mcpp 单独报告耗时。以下按 `std-module.json` 中记录的原始
编译命令在临时目录单独执行三次，均为 `-O2`，结果由 dev/release 共享；
它们也已包含在上表每次“全局冷”的总时间内，不应再相加。

| std 单独编译 | 中位秒数 |
| --- | ---: |
| 宿主 GCC 16.2.1 | 2.629 |
| 受管理 GCC 16.1.0 | 2.610 |

本次完整重跑共 90 个计时样本，98 次命令执行，检测到 0 次休眠跨越；各中位数均来自三个通过双时钟检查且退出 0 的样本。

这些比较仍有边界：xmake release 为 `-O3 -DNDEBUG`，并使用 hidden visibility；
mcpp release 为 `-O2`，dev 双方都是 `-O0 -g`。mcpp 给普通 C++ 单元加 `-fPIC`，
std 无论 dev/release 均单独以 `-O2` 缓存；xmake 的 std 随目标使用该模式的编译参数。
xmake 在两个 socket 测试各编译一次 std 和 socket 模块，mcpp 图中各编译一次；
相反 mcpp 额外构建 JSON 模块。xmake 的 socket 测试也继承了 su_log 依赖。
因此本表比较相同应用源码/测试集合的现有构建图，不是相同编译命令的调度器微基准。
只 touch 接口不等于真实修改接口，也不能据此推断大项目或完整应用的速度。
未清空操作系统页缓存、固定 CPU 调频策略或随机化工具顺序；这些是同机小样本的
描述性中位数，不是统计显著性或通用加速结论。

即便使用同一个宿主编译器，mcpp 仍选择自己的 glibc 2.44.3、loader 和运行库
链接规则；xmake 使用宿主环境。mcpp 声明 self-contained C++ runtime 不意味着
Linux exe 全静态或脱离 glibc；不能把这里的测试产物直接当发布包。

建议保持可选，用于核心库隔离、模块兼容验证和独立测试；暂不扩大到整套应用。
同一宿主 GCC 下，mcpp 的无改动检查约 0.10 秒，热全局缓存下冷项目构建也较快；
完全冷缓存的 debug 基本持平，日志头文件增量则比 xmake 慢，socket 增量没有一致优势。
release 还有 O2/O3 差别。结合系统运行库、外部 SDK 与生成规则的集成成本，
当前证据不足以支持仅为编译速度替换 xmake。受管理 GCC 的 socket 编译较快也不能
归因于 mcpp 本身，因为编译器版本发生了变化。

验证结果：原始 xmake 的 debug/release 各 5/5 通过；mcpp 的受管理与宿主 GCC
在 dev/release 各 5/5 通过；Windows 受管理 MinGW 三个测试交叉编译成功且 Wine
运行 3/3 通过；计时器回归测试 3/3 通过。未推送、未创建 PR。

参考 mcpp 官方仓库 docs/04-mcpp-toml.md、07-workspace.md、08-testing.md、
20-toolchains.md、22-target-side.md（本次安装版本 2026.10.21.1）；
工具链 path、cache off 与运行库结论以本机实际日志及评审补充为准。
