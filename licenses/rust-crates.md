# Rust 依赖许可证清单

由两个交付组件的 `Cargo.lock` 生成，并逐个对照本地 registry 源码包内 `Cargo.toml`
的 `license` 字段核对（核对日期 2026-09-28）：`src/core-rs`（Linux/PAM 核心）与
`src/platform/windows/credential_provider_rs`（Windows 凭据提供者）。

多重许可（OR）时，本发行选择 MIT 一方履行义务（memchr 的 Unlicense OR MIT 同理）；
许可证全文见本目录 `rust/` 子目录。

## MIT OR Apache-2.0 (MIT chosen)（63 个）

| crate | 上游仓库 |
| --- | --- |
| aead 0.5.2 | https://github.com/RustCrypto/traits |
| block-buffer 0.10.4 | https://github.com/RustCrypto/utils |
| cfg-if 1.0.4 | https://github.com/rust-lang/cfg-if |
| chacha20 0.9.1 | https://github.com/RustCrypto/stream-ciphers |
| chacha20poly1305 0.10.1 | https://github.com/RustCrypto/AEADs/tree/master/chacha20poly1305 |
| cipher 0.4.4 | https://github.com/RustCrypto/traits |
| cpufeatures 0.2.17 | https://github.com/RustCrypto/utils |
| crypto-common 0.1.7 | https://github.com/RustCrypto/traits |
| digest 0.10.7 | https://github.com/RustCrypto/traits |
| equivalent 1.0.2 | https://github.com/indexmap-rs/equivalent |
| generic-array 0.14.7 | https://github.com/fizyk20/generic-array.git |
| getrandom 0.2.17 | https://github.com/rust-random/getrandom |
| getrandom 0.3.4 | https://github.com/rust-random/getrandom |
| hashbrown 0.17.1 | https://github.com/rust-lang/hashbrown |
| hkdf 0.12.4 | https://github.com/RustCrypto/KDFs/ |
| hmac 0.12.1 | https://github.com/RustCrypto/MACs |
| indexmap 2.14.0 | https://github.com/indexmap-rs/indexmap |
| inout 0.1.4 | https://github.com/RustCrypto/utils |
| itoa 1.0.18 | https://github.com/dtolnay/itoa |
| libc 0.2.186 | https://github.com/rust-lang/libc |
| libc 0.2.189 | https://github.com/rust-lang/libc |
| opaque-debug 0.3.1 | https://github.com/RustCrypto/utils |
| poly1305 0.8.0 | https://github.com/RustCrypto/universal-hashes |
| proc-macro2 1.0.106 | https://github.com/dtolnay/proc-macro2 |
| proc-macro2 1.0.107 | https://github.com/dtolnay/proc-macro2 |
| quote 1.0.46 | https://github.com/dtolnay/quote |
| quote 1.0.47 | https://github.com/dtolnay/quote |
| rand_core 0.6.4 | https://github.com/rust-random/rand |
| serde 1.0.228 | https://github.com/serde-rs/serde |
| serde_core 1.0.228 | https://github.com/serde-rs/serde |
| serde_derive 1.0.228 | https://github.com/serde-rs/serde |
| serde_json 1.0.150 | https://github.com/serde-rs/json |
| serde_spanned 1.1.1 | https://github.com/toml-rs/toml |
| sha2 0.10.9 | https://github.com/RustCrypto/hashes |
| syn 2.0.118 | https://github.com/dtolnay/syn |
| syn 2.0.119 | https://github.com/dtolnay/syn |
| toml 0.9.12+spec-1.1.0 | https://github.com/toml-rs/toml |
| toml_datetime 0.7.5+spec-1.1.0 | https://github.com/toml-rs/toml |
| toml_parser 1.1.2+spec-1.1.0 | https://github.com/toml-rs/toml |
| toml_writer 1.1.1+spec-1.1.0 | https://github.com/toml-rs/toml |
| typenum 1.20.1 | https://github.com/paholg/typenum |
| universal-hash 0.5.1 | https://github.com/RustCrypto/traits |
| version_check 0.9.5 | https://github.com/SergioBenitez/version_check |
| winapi 0.3.9 | https://github.com/retep998/winapi-rs |
| winapi-i686-pc-windows-gnu 0.4.0 | https://github.com/retep998/winapi-rs |
| winapi-x86_64-pc-windows-gnu 0.4.0 | https://github.com/retep998/winapi-rs |
| windows 0.62.2 | https://github.com/microsoft/windows-rs |
| windows-collections 0.3.2 | https://github.com/microsoft/windows-rs |
| windows-core 0.62.2 | https://github.com/microsoft/windows-rs |
| windows-future 0.3.2 | https://github.com/microsoft/windows-rs |
| windows-implement 0.60.2 | https://github.com/microsoft/windows-rs |
| windows-interface 0.59.3 | https://github.com/microsoft/windows-rs |
| windows-link 0.2.1 | https://github.com/microsoft/windows-rs |
| windows-numerics 0.3.1 | https://github.com/microsoft/windows-rs |
| windows-result 0.4.1 | https://github.com/microsoft/windows-rs |
| windows-strings 0.5.1 | https://github.com/microsoft/windows-rs |
| windows-sys 0.61.2 | https://github.com/microsoft/windows-rs |
| windows-threading 0.2.1 | https://github.com/microsoft/windows-rs |
| winnow 0.7.15 | https://github.com/winnow-rs/winnow |
| winnow 1.0.3 | https://github.com/winnow-rs/winnow |
| zeroize 1.9.0 | https://github.com/RustCrypto/utils |
| zeroize_derive 1.5.0 | https://github.com/RustCrypto/utils |
| zmij 1.0.21 | https://github.com/dtolnay/zmij |

## Apache-2.0 WITH LLVM-exception OR Apache-2.0 OR MIT（3 个）

| crate | 上游仓库 |
| --- | --- |
| wasi 0.11.1+wasi-snapshot-preview1 | https://github.com/bytecodealliance/wasi-rs |
| wasip2 1.0.4+wasi-0.2.12 | https://github.com/bytecodealliance/wasi-rs |
| wit-bindgen 0.57.1 | https://github.com/bytecodealliance/wit-bindgen |

## (MIT OR Apache-2.0) AND Unicode-3.0（1 个）

| crate | 上游仓库 |
| --- | --- |
| unicode-ident 1.0.24 | https://github.com/dtolnay/unicode-ident |

## Unlicense OR MIT（1 个）

| crate | 上游仓库 |
| --- | --- |
| memchr 2.8.2 | https://github.com/BurntSushi/memchr |

## MIT OR Apache-2.0 OR LGPL-2.1-or-later (MIT chosen)（1 个）

| crate | 上游仓库 |
| --- | --- |
| r-efi 5.3.0 | https://github.com/r-efi/r-efi |

## BSD-3-Clause（1 个）

| crate | 上游仓库 |
| --- | --- |
| subtle 2.6.1 | https://github.com/dalek-cryptography/subtle |
