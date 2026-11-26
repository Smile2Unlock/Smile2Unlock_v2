# CI scope

The ordinary PR workflow uses a fixed set of broad checks:

- Linux: Rust formatting/core tests, product binaries, `su_about_test`, the
  C++/Rust face-auth smoke test, LightFace preprocessing and ONNX pipeline smoke
  tests, and tarball verification.
- Windows: service/helper/password-tool/credential-provider/recognition-agent
  builds (`--with_lightface=y`), helper startup, LightFace preprocessing and the
  Rust credential-provider suite under Wine.
- Full signed Windows package verification is a manual `workflow_dispatch`
  job. Tag publishing still builds and verifies the release packages.

Workflows check out with `lfs: false` (no Git LFS) and populate cached LightFace
ONNX weights via `packaging/models/fetch-models.sh` from
`Smile2Unlock/LightFacePipeline` Releases; all release archives embed the
models by default. Xmake package caches can restore across target/test changes
to `xmake.lua`. Both `local-repo/**` and `xmake/repo/**` are part of the cache
key/restore prefix, so recipe changes invalidate the dependency cache.

Issue-specific regressions stay in the repository for focused local runs.
Camera mode/mailbox tests, Unicode paths/models/usernames, initials, About
licenses, themes, service rate limiting/worker/disconnect cases and Linux
root-versus-nobody fixture packaging are not separate checks on every PR.
The broad Rust suites continue to exercise their existing regression cases.

Test executables and acceptance/probe tools have `set_default(false)`, so a
plain `xmake build` builds the product without compiling every test. Build and
run a test explicitly when working on the corresponding area, for example:

```bash
xmake build su_capture_pipeline_test
xmake test su_capture_pipeline_test/default
```

Other focused targets include `su_windows_unicode_paths_test`,
`su_windows_unicode_models_test`, `su_windows_deploy_helper_result_test`,
`su_seetaface_pipeline_smoke_test`, `su_deploy_test` and
`su_control_socket_smoke_test`. They require the matching platform and build
options; `su_seetaface_pipeline_smoke_test` and `su_windows_unicode_models_test`
require `--with_seetaface=y`. The packaging fixture test remains available as
`packaging/testing/linux-package-smoke.sh`.

Use focused checks to validate a fix without adding another mandatory CI job
or per-issue workflow step. Change the ordinary suite when its broad scope
needs to change, rather than growing it for every bug.
