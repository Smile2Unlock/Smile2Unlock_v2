# 第三方许可证

本目录收录发行物中链接或内嵌的全部第三方组件许可证（核对日期 2026-09-28，
均按上游仓库/官方发布物逐字核对，不再保留未使用的旧条目）。项目的自有代码见根目录
`LICENSE`（MIT，Smile2Unlock contributors）。

## C/C++ 库（xmake 依赖）

| 组件 | 版本 | 许可证 | 上游 | 文件 |
| --- | --- | --- | --- | --- |
| Slint | 1.17.0 | 三重许可：GPL-3.0 / Royalty-free 2.0 / Slint Software 3.0，另有内嵌第三方资源（字体 OFL、qskinny 等） | https://github.com/slint-ui/slint | `slint/`（官方包自带 LICENSE.md + LICENSES/ 全套） |
| SeetaFace6（seetaface6open，含随包模型） | xmake `latest` | BSD-2-Clause | https://github.com/SeetaFace6Open | `SeetaFace6-BSD-2-Clause.txt` |
| libpng | 1.6.58 | PNG Reference Library License v2 | https://github.com/pnggroup/libpng | `libpng-PNG-Reference-Library-v2.txt` |
| libjpeg-turbo | 3.1.4 | IJG 许可 + BSD-3-Clause（双许可，另含被覆盖的 zlib、PNG-ref-v2 条款） | https://github.com/libjpeg-turbo/libjpeg-turbo | `libjpeg-turbo-LICENSE.md`、`libjpeg-turbo-README.ijg.txt` |
| CImg | 4.0.4 | CeCILL-C 或 CeCILL v2（双许可） | https://github.com/dtschump/CImg | `CImg-CeCILL-C.txt`、`CImg-CeCILL-V2.txt` |
| libyuv | 1913 | BSD-3-Clause | https://chromium.googlesource.com/libyuv/libyuv | `libyuv-BSD-3-Clause.txt` |
| nlohmann_json | 3.12.0 | MIT | https://github.com/nlohmann/json | `nlohmann_json-MIT.txt` |

## Rust 库

| 组件 | 许可证 | 来源 | 文件 |
| --- | --- | --- | --- |
| memsafe（vendored，含本项目补丁的 fork） | MIT（pouyan shalbafan） | https://crates.io/crates/memsafe | `memsafe-vendored-MIT.txt`（上游原文；fork 修改以本仓库 git 历史为准） |
| registry 依赖共 70 个 | 见分组清单 | 各自上游 | `rust-crates.md`（清单）+ `rust/`（全文） |

`rust/` 内为去重后的许可证全文：`MIT.txt`、`Apache-2.0.txt`、`BSD-3-Clause.txt`
（subtle）、`Unicode-3.0.txt`（unicode-ident 的 AND 条款）、
`Apache-2.0-LLVM-exception.txt`（wasi 系）。

## 发行义务提示

- libjpeg-turbo 二进制分发需在文档中注明 "This software is based in part on the
  work of the Independent JPEG Group."（见其 LICENSE.md 汇总）。
- Slint 为三重许可，发行前需确认项目实际采用的许可方（Royalty-free 2.0 的适用
  条件或 GPL-3.0），本目录保留全部候选文本与内嵌资源许可。
- CeCILL 系为法语法律文本的官方英文翻译版本，按其条款以法文原文为准。
