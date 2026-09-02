# Smile2Unlock DMS Monet Theme Integration Plan

## Summary

本计划用于让 Linux 端 UI 跟随 DankMaterialShell（DMS）的 Material 3 / Monet 配色，同时保留桌面环境无关的稳定回退路径。

实现时优先复用 DMS 已经生成的完整 Material 3 色板，不重复从壁纸推导颜色；只有 DMS 色板不可用时，才通过当前壁纸和 `matugen` 生成临时色板。所有主题输入最终转换为 Smile2Unlock 自己的语义化主题模型，再由 C++ 在 Slint 事件循环中更新 UI。

本文最初用于描述实现方案；当前实现状态记录如下。

## Implementation Status

Phase 1 至 Phase 5 已完成：Slint 使用应用自身的语义主题角色，Linux 端会优先加载 DMS Material 3 色板，支持 DMS 会话模式、目录级实时刷新，以及通过受控参数调用 Matugen 的壁纸取色回退。成功、提醒和错误状态分别使用 Material 3 的 tertiary、secondary 和 error 容器角色，并进行前景对比度校正；不再在动态主题中固定使用绿色或黄色。所有外部来源不可用时使用内置主题。

设置页现已提供 `跟随系统 / 浅色 / 深色` 和 `自动 / 显示 / 隐藏` 原生窗口按键偏好。自动模式在 Niri、Sway、Hyprland、i3、dwm 等独立窗口管理器下隐藏窗口装饰，在 GNOME、KDE Plasma 等完整桌面环境和未知环境下保留原生最小化、最大化、关闭按键。当前实现不会修改 DMS 或桌面配置。

## Goals

- Linux 端在 DMS 环境中自动使用当前 Monet 配色，并正确跟随深色 / 浅色模式。
- UI 组件只依赖 Smile2Unlock 的语义色名，不直接依赖 DMS JSON 字段或某一张壁纸的具体颜色。
- DMS 缺失、缓存损坏、壁纸不可读或 `matugen` 不可用时，应用仍能使用内置主题正常启动。
- 主题变化可以在运行时刷新，不要求重启应用。
- 状态、成功、警告和失败信息保持足够对比度，不因动态取色失去业务语义。
- 主题适配保持在 Linux 平台边界内，不影响 Windows UI 的现有行为。

## Non-goals

- 本阶段不重写 DMS，也不修改 DMS 的缓存或配置。
- 不把 DMS 作为 Smile2Unlock 的强制运行依赖。
- 不直接照搬旧版 UI 的颜色层级。
- 不提供完整的用户自定义主题编辑器。
- 不在第一阶段适配所有桌面 Shell 的私有主题接口。
- 不让 Slint 直接读取文件、执行命令或解析外部 JSON。

## Current DMS Capabilities

当前环境使用 DMS `v1.5.1`，已确认以下接口和文件：

| 能力 | 接口 / 路径 | 结论 |
| --- | --- | --- |
| 当前深浅模式 | `dms ipc call theme getMode` | 可用，返回 `dark` 或 `light` |
| 当前壁纸 | `dms ipc call wallpaper get` | 可用，返回壁纸路径 |
| 完整 Material 3 色板 | `~/.cache/DankMaterialShell/dms-colors.json` | 可用，包含 dark / light 色板 |
| DMS 会话状态 | `~/.local/state/DankMaterialShell/session.json` | 可用，可作为模式状态来源 |
| XDG 深浅模式 | `org.freedesktop.appearance color-scheme` | 可用作非 DMS 环境的辅助判断 |
| 完整色板 IPC / D-Bus API | 未发现 | 不能作为主路径 |
| 本机 `matugen` | 已安装 | 可作为壁纸取色回退 |

DMS 内部也通过 `/usr/share/quickshell/dms/Common/Theme.qml` 消费 `dms-colors.json`。因此，读取该缓存比再次对壁纸运行 `matugen` 更接近用户在 DMS 中实际看到的配色。DMS 可能对 surface 类颜色做额外调整，例如当前 DMS 的 dark `surface_container` 为 `#131318`，而直接运行 `matugen` 得到的是 `#1f1f25`。

## Theme Source Priority

主题加载按以下优先级进行：

1. 读取 DMS 的 `dms-colors.json`，并根据 DMS 当前模式选择 dark 或 light 色板。
2. DMS 色板不可用时，通过 DMS IPC 获取当前壁纸，使用 `matugen --dry-run` 生成内存中的临时色板。
3. DMS IPC 不可用时，尝试桌面通用来源获取壁纸和深浅模式；GNOME 通过 `gsettings` 的 `picture-uri` / `picture-uri-dark` 获取本地壁纸，只有获得可靠路径后才运行 `matugen`。
4. 任一外部步骤失败时，使用 Smile2Unlock 内置的深色或浅色主题。

外部主题加载失败不得阻止应用启动，也不得覆盖或改写 DMS 缓存。

推荐的 Matugen 回退命令：

```bash
matugen image "$wallpaper" \
  --dry-run -j hex \
  -m dark \
  -t scheme-tonal-spot \
  --contrast 0 \
  --source-color-index 0
```

实现时不应通过 shell 拼接命令。C++ 侧应使用参数数组启动子进程，分别传递壁纸路径、模式和固定选项，避免路径转义和命令注入问题。

## Proposed Architecture

主题功能分为四层：

```text
DMS cache / DMS IPC / desktop portal / matugen
  -> Linux ThemeSource adapters
  -> ThemePalette parser + validation
  -> AppTheme semantic model
  -> AppController -> Slint Theme global
```

### ThemeSource

Linux 平台适配层负责所有外部副作用：

- 定位并读取 DMS 色板和会话文件。
- 调用 DMS IPC 获取模式或壁纸。
- 必要时读取 XDG portal 的深浅模式。
- 必要时以受控参数调用 `matugen --dry-run`。
- 监听外部文件变化并请求重新加载。

每个来源返回类型化结果，不直接修改 UI。来源探测和优先级选择应集中管理，避免把 DMS 判断散落到控制器和组件中。

### ThemePalette

解析层把外部 JSON 转换为包含完整颜色角色的不可变值对象，并完成：

- 必需字段检查。
- `#RRGGBB` / `#AARRGGBB` 格式解析。
- dark / light scheme 选择。
- 缺失字段的同层级回退。
- 前景与背景的最低对比度检查。

JSON 解析应使用项目已有的结构化解析能力；如果现有依赖不能满足需求，再选择体积小、维护稳定的解析方案，避免手写字符串扫描。

### AppTheme

应用层只暴露 UI 实际需要的语义角色。外部色板字段先映射为 `AppTheme`，Slint 不感知 DMS、Matugen 或 JSON 的存在。

建议的基础映射：

| Smile2Unlock role | Material 3 role | 用途 |
| --- | --- | --- |
| `window_background` | `background` | 应用窗口背景 |
| `panel_background` | `surface_container_lowest` | 主要内容区域 |
| `control_background` | `surface_container` | 输入框、列表和普通控件 |
| `control_background_active` | `surface_container_high` | 悬停、选中和强调层级 |
| `accent` | `primary` | 主要操作和焦点 |
| `accent_container` | `primary_container` | 低强调度的品牌区域 |
| `on_accent` | `on_primary` | 主色上的文本和图标 |
| `text_primary` | `on_surface` | 主要文本 |
| `text_secondary` | `on_surface_variant` | 次要文本和说明 |
| `border` | `outline_variant` | 分隔线和普通边框 |
| `danger` | `error` | 失败和危险操作 |
| `danger_container` | `error_container` | 错误提示背景 |

成功、警告和识别失败状态不能全部简单映射到 `primary`。当前实现使用 Material 3 的 `tertiary_container` / `on_tertiary_container` 表示成功，使用 `secondary_container` / `on_secondary_container` 表示提醒，并继续使用 `error_container` 表示失败；每组前景色都会进行对比度校正。色板缺少扩展角色时，分别回退到同一色板的 `primary_container` 和高层级 surface，不再回退到固定的绿色或黄色。

### Slint Theme Global

在 Slint 中建立单一的主题 global，集中提供颜色属性。现有组件改为引用该 global，禁止在页面组件中继续增加业务无关的颜色字面量。

C++ 负责把完整 `AppTheme` 一次性投递到 Slint 事件循环。更新过程中应避免逐字段触发可见的中间状态；如果 Slint 绑定方式不能原子替换结构体，则先在 C++ 中生成完整快照，再在同一个事件循环回调中更新全部属性。

## Dark And Light Mode

选择 `跟随系统` 时，模式来源优先级：

1. DMS IPC 的 `theme getMode`。
2. DMS `session.json` 中的当前模式。
3. XDG portal `org.freedesktop.appearance color-scheme`。
4. 内置默认模式。

设置页提供 `跟随系统 / 浅色 / 深色` 三种模式，默认使用 `跟随系统`。显式浅色或深色优先于上述系统来源，只改变从色板中选择的 scheme，不修改 DMS 或桌面设置。

## Native Window Controls

窗口按键使用 Slint 的原生窗口装饰，不在应用内容区自绘最小化、最大化和关闭按钮。设置页提供以下模式：

- `自动`：根据桌面会话环境判断。独立窗口管理器默认无边框，完整桌面环境及未知环境默认保留原生装饰。
- `显示`：强制请求系统原生窗口装饰。
- `隐藏`：强制使用无边框窗口。

该偏好与主题模式、语言一起保存在用户级 `~/.config/smile2unlock/ui.json`（遵循 `XDG_CONFIG_HOME`），不会生成或修改 Niri、dwm 或其他窗口管理器配置。

## Live Refresh

运行时刷新至少监听：

- `~/.cache/DankMaterialShell/dms-colors.json`
- `~/.local/state/DankMaterialShell/session.json`

监听器需要处理 DMS 常见的原子替换写入方式，而不是只监听原 inode。应监听父目录并按文件名过滤，同时对短时间内的连续事件做 debounce。

推荐刷新流程：

1. 文件事件只发出“主题可能变化”的通知。
2. 后台任务重新读取并解析完整色板。
3. 解析成功后生成新的不可变 `AppTheme`。
4. 与当前主题比较；无变化则跳过。
5. 通过 Slint 事件循环应用新快照。
6. 解析失败时保留当前有效主题，并记录一次有上下文的警告。

文件监听功能不可用时，应用仍可在启动时加载主题；不应为此引入高频轮询。

## Failure Handling

- 文件不存在：继续尝试下一优先级来源。
- JSON 不完整或颜色非法：拒绝整个候选快照，保留上一次有效主题。
- DMS IPC 超时：快速失败并进入回退路径，不能阻塞 UI 启动。
- 壁纸路径不可读：跳过 Matugen，使用内置主题。
- `matugen` 未安装或退出失败：使用内置主题。
- 动态颜色对比度不足：为问题角色选择高对比度前景色，而不是拒绝整张动态色板。
- 运行中外部来源消失：保留当前主题；只有用户模式变化或下一次有效主题出现时再更新。

日志中只记录来源类型、失败阶段和必要的文件路径，不输出无关环境变量或完整外部命令行。

## Implementation Phases

### Phase 1: Semantic Theme Foundation

- 盘点 `src/app/ui/` 中现有颜色字面量及其用途。
- 定义 `AppTheme` 和内置 dark / light 主题。
- 建立 Slint theme global，并迁移现有组件。
- 保证默认视觉与当前 UI 接近，避免同时进行第二轮布局重写。

### Phase 2: DMS Palette Loading

- 实现 DMS 路径定位、JSON 解析和字段验证。
- 实现 DMS 模式读取及 dark / light scheme 选择。
- 把 DMS 色板映射为 `AppTheme`。
- 在应用启动时加载一次，失败时使用内置主题。

### Phase 3: Live Refresh

- 增加目录级文件监听和 debounce。
- 在后台重载主题，在 Slint 事件循环中应用。
- 覆盖原子替换、连续更新、损坏后恢复等情况。

### Phase 4: Wallpaper And Matugen Fallback

- 实现 DMS 壁纸 IPC 适配器。
- 以参数数组调用 `matugen --dry-run` 并解析标准输出。
- 增加桌面通用的模式来源；壁纸来源只在接口可靠时接入。
- 明确进程超时、输出大小限制和错误回退。

### Phase 5: Settings And Additional Desktops

- [x] 在设置页加入 `跟随系统 / 浅色 / 深色`，支持运行时切换和持久化。
- [x] 加入 `自动 / 显示 / 隐藏` 原生窗口按键设置；独立窗口管理器自动隐藏，其他环境默认显示。
- [x] 使用 GNOME 稳定的 `gsettings` 壁纸键作为非 DMS 回退来源。
- [x] 保持上层 `AppTheme` 合约不变；KDE 暂不解析 Plasma 私有配置，因为目前没有找到稳定的只读壁纸接口。

## Tests

### Unit Tests

- 解析有效的 DMS dark / light 色板。
- 拒绝缺失字段、非法 hex、错误 JSON 类型和超大输入。
- 验证 Material 3 role 到 `AppTheme` 的映射。
- 验证来源优先级和每一级失败后的回退。
- 验证显式模式覆盖和系统模式选择。
- 验证窗口按键自动判断及显式显示 / 隐藏覆盖。
- 验证旧版语言偏好文件可兼容升级，保存任一设置不会覆盖其他字段。
- 验证相同主题不会触发重复 UI 更新。
- 验证低对比度颜色会使用安全语义色回退。

### Integration Tests

- 使用临时 HOME / XDG 目录注入 DMS 色板和状态文件。
- 模拟文件原子替换、连续写入、损坏后恢复。
- 使用可控的 fake IPC / process runner 验证超时和 Matugen 参数，不依赖开发机真实 DMS。
- 在没有 DMS 和 Matugen 的环境中启动应用。
- 执行 `xmake build` 和现有测试目标，确认 Linux 主线构建不退化。

### Visual Verification

- 分别检查深色和浅色主题下的主界面、用户列表、录入流程、设置页和错误状态。
- 检查窄窗口、默认窗口和高 DPI 下的文本与控件对比度。
- 使用高亮、低饱和、接近黑色和接近白色的壁纸色板验证可读性。
- 确认焦点、禁用、悬停、选中、成功、警告和失败状态可区分。

## Acceptance Criteria

- DMS 运行且缓存有效时，Smile2Unlock 使用与 DMS 一致的 dark / light Material 3 色板。
- DMS 模式或色板变化后，运行中的 UI 能稳定刷新且无明显闪烁。
- DMS、IPC、壁纸或 Matugen 任一项缺失时，应用仍能启动并保持可读。
- UI 层不存在对 DMS 文件格式、IPC 命令或 Matugen 输出的直接依赖。
- 动态色板不会降低认证结果和危险操作的语义辨识度。
- 自动测试不依赖当前用户的真实 DMS 配置。
- 独立窗口管理器默认无原生装饰，其他桌面环境默认保留原生窗口按键，且用户可以显式覆盖。

## Suggested Commit Breakdown

实现阶段建议继续拆分提交：

1. `refactor: centralize Slint theme roles`
2. `feat: load Material palette from DMS`
3. `feat: refresh DMS theme at runtime`
4. `feat: derive fallback theme from wallpaper`
5. `feat: add UI theme mode preference`

每个提交都应保持 `xmake build` 可通过，并只在对应功能完成后加入测试，避免主题基础设施、外部集成和设置 UI 混在同一个提交中。
