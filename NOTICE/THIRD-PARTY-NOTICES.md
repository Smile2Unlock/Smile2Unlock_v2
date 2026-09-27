# 第三方组件许可证声明

本项目根据 MIT 许可证发布（见 LICENSE 文件）。本文件列出发行物中实际链接或内嵌的
第三方组件及其许可证；每项的许可证原文随发行物一并提供，亦见仓库 `licenses/`
目录（含索引 `licenses/README.md`）。核对日期：2026-09-28，所有条款均按上游
仓库/官方发布物核对，版权行逐字摘自对应上游文件。

## C/C++ 组件

   **Slint** v1.17.0 (https://github.com/slint-ui/slint)
   - 许可证: 三重许可 — GPL-3.0-only / Slint Royalty-free Desktop, Mobile, and
     Web Applications License 2.0 / Slint Software License 3.0（发行方按自身适用
     条件择一履行）
   - 版权:

    Copyright (c) SixtyFPS GmbH

   - 另含内嵌第三方资源（字体 OFL-1.1、qskinny 等），原文见 `licenses/slint/`

   **SeetaFace6**（seetaface6open，含随包 .csta 模型）
   (https://github.com/SeetaFace6Open/index)
   - 许可证: BSD-2-Clause (SPDX: BSD-2-Clause)
   - 版权:

    Copyright (c) 2019, SeetaTech, Institute of Computing Technology, Chinese Academy of Sciences, Beijing, China

   **libpng** v1.6.58 (https://github.com/pnggroup/libpng)
   - 许可证: PNG Reference Library License version 2
   - 版权:

    Copyright (c) 1995-2026 The PNG Reference Library Authors.
    Copyright (c) 2018-2026 Cosmin Truta.
    Copyright (c) 2000-2002, 2004, 2006-2018 Glenn Randers-Pehrson.
    Copyright (c) 1996-1997 Andreas Dilger.
    Copyright (c) 1995-1996 Guy Eric Schalnat, Group 42, Inc.

   **libjpeg-turbo** v3.1.4 (https://github.com/libjpeg-turbo/libjpeg-turbo)
   - 许可证: IJG 许可 与 Modified BSD-3-Clause 双许可（zlib 与 PNG Reference
     Library v2 条款按其汇总被覆盖）
   - 版权:

    Copyright (C) 2009-2026 D. R. Commander
    Copyright (C) 2018-2023 Randy <randy408@protonmail.com>

   - 另继承 Independent JPEG Group 的 libjpeg 版权（见 README.ijg）
   - 二进制分发声明（IJG 条款要求）: This software is based in part on the work
     of the Independent JPEG Group.

   **CImg** v4.0.4 (https://github.com/dtschump/CImg)
   - 许可证: CeCILL-C 或 CeCILL v2.0 双许可（法文原文为准，随附官方英文译本）
   - 版权（摘自 CImg.h 头部署名，完整贡献者名单见上游 README.txt）:

    Project manager: David Tschumperlé (http://tschumperle.users.greyc.fr/)

   **libyuv** (https://chromium.googlesource.com/libyuv/libyuv)
   - 许可证: BSD-3-Clause (SPDX: BSD-3-Clause)
   - 版权:

    Copyright 2011 The LibYuv Project Authors. All rights reserved.

   **nlohmann_json** v3.12.0 (https://github.com/nlohmann/json)
   - 许可证: MIT (SPDX: MIT)
   - 版权:

    Copyright (c) 2013-2025 Niels Lohmann

## Rust 组件

   **memsafe**（vendored fork，含本项目的安全加固补丁）
   (https://crates.io/crates/memsafe)
   - 许可证: MIT (SPDX: MIT)
   - 版权:

    Copyright (c) 2025 pouyan shalbafan

   **registry 依赖（70 个 crate）**
   - 许可证: 以 MIT / Apache-2.0 双许可为主，另含 subtle (BSD-3-Clause)、
     unicode-ident（附加 Unicode-3.0）、wasi 系（Apache-2.0 WITH LLVM-exception
     可选）等
   - 版权: 各 crate 的版权声明见其许可证文件；完整清单与全文见
     `licenses/rust-crates.md` 与 `licenses/rust/`

## 运行环境依赖

Linux 发行物动态链接发行版提供的系统库（glibc、wayland/compositor 相关库、
libstdc++ 等），其许可由发行版提供，不在本文件范围内。Windows 发行物仅依赖
操作系统组件（DirectShow/Media Foundation、NCrypt/BCrypt、LSA 等），随 Windows
授权提供。
