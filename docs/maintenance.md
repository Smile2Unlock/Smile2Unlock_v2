# 维护手册

本文面向开发者和维护者，记录日常构建、检查、文档维护和正式发布的入口。

## 开发环境

项目使用 Xmake、C++26 modules、Rust 和 Slint。ONNX Runtime 由项目内置
`xmake/repo/` 提供，默认人脸模型通过 `bash packaging/models/fetch-models.sh`
从 `Smile2Unlock/LightFacePipeline` Release 拉取（无需 Git LFS）；Slint 与可选
SeetaFace6 配方由 `local-repo` 提供，Rust/Slint 工具链初始化脚本位于
`packaging/slint/`。

Linux 本地构建：

```bash
bash packaging/slint/setup-rust.sh
bash packaging/models/fetch-models.sh
xmake f -y -p linux -a x86_64 -m release
xmake build -j2 su_app
```

运行桌面程序并进行界面检查时，使用 Xvfb 和隔离的 XDG 目录，避免读写真实桌面配置：

```bash
tmp="$(mktemp -d)"
mkdir -p "$tmp/config" "$tmp/cache" "$tmp/data"
XDG_CONFIG_HOME="$tmp/config" XDG_CACHE_HOME="$tmp/cache" \
XDG_DATA_HOME="$tmp/data" xvfb-run -a build/linux/x86_64/release/su_app
```

运行具体领域的测试前，先查看 [`ci.md`](ci.md) 中的目标和依赖说明。测试目标默认不随普通 `xmake build` 编译。

## 版本与发布

正式产品版本以 Git tag `vMAJOR.MINOR.PATCH` 为准。发布顺序：

1. 通过正常评审和分支保护将代码合入 `main`。
2. 确认工作树干净、目标提交和发布说明正确，再创建并推送版本 tag，例如 `v2.4.2`。
3. `publish-release-artifacts.yml` 校验 tag 格式，将 `SU_VERSION=2.4.2` 注入 Linux/Windows 构建和打包，并创建 GitHub Release。
4. 核对工作流产物、SHA-256、签名/验证结果和 Release 附件。tag 不匹配或产物校验失败时，不要手工替换附件。

分支保护不需要为版本号单独开 PR。未设置 `SU_VERSION` 的开发构建使用 `version.txt`
作为基线；发布构建必须由 tag 明确提供版本。Rust crate 版本与 core ABI 版本独立管理。

## 文档维护

- 两份 README 是项目入口，保持中英文的主要能力、平台边界、截图和链接同步。
- [`features.md`](features.md) 记录已实现的用户可见设置；增加、删除或改变设置行为时同步更新该表。
- [`current_status.md`](current_status.md) 记录验证快照和未完成的发布/现场验收项。所有带日期的验证结果都必须保留原日期，新增结果另起条目。
- `*_plan.md`、`*_verification.md` 和平台验收报告记录设计或历史证据；不要把旧计划中的未勾选项直接当作当前缺陷，也不要覆盖历史验收结论。
- 新增文档时更新 README 或本手册中的导航。删除、移动文档前检查仓库内链接。
- UI 截图通过 Xvfb 捕获，使用临时 `XDG_CONFIG_HOME`、`XDG_CACHE_HOME` 和 `XDG_DATA_HOME`；截图不得包含真实用户资料或真实桌面内容。

## 发布检查

- 在目标平台重新构建当前提交，不复用旧的 `build/` 产物作为发布证据。
- 运行与变更相关的 Rust/Xmake 测试和包验证；完整 CI 范围见 [`ci.md`](ci.md)。
- 对 Windows/Linux 登录集成变更，按 [`release_acceptance_checklist.md`](release_acceptance_checklist.md) 检查对应原生系统和回退路径。
- 记录本地验证与真实系统验收的边界；构建通过不能替代摄像头、greeter、LogonUI 或 PAM 现场验收。
- 检查版本来源、包内 `release-info.json`、文件哈希和发布附件命名一致。
