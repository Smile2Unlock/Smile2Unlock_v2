add_rules("mode.debug", "mode.release")

-- Xmake's repository syntax is "<name> <path>"; both tokens are required.
add_repositories("local-repo local-repo")

-- Project identity + version. CI may override SU_VERSION; the literal
-- fallback is synchronized with version.txt by bump-version.sh because
-- project files do not expose file I/O at top level.
local _su_version = os.getenv("SU_VERSION")
if not _su_version or _su_version == "" then
    _su_version = "2.3.0"
end
set_version(_su_version, {build = "", arch = os.arch()})
set_description("Smile2Unlock - local face authentication (Windows sign-in + Linux PAM)")
-- Package metadata and per-file hashes are embedded in each package's
-- release-info.json; xmake's project API has no setters for the remaining
-- project metadata in this version.


add_requires("nlohmann_json v3.12.0", { system = false })
add_requires("cimg v4.0.4")
-- Static libyuv avoids a runtime dependency on the distro's libyuv.so,
-- which is not present on many distributions (Arch/Debian/Fedora shipping
-- different sonames or none at all). Built with JPEG (MJPEG decode) from the
-- local-repo package; mingw builds pull libjpeg-turbo automatically.
add_requires("libyuv", { system = false, configs = { shared = false, jpeg = true, jpeg_library = "libjpeg-turbo" } })

set_encodings("utf-8")
set_languages("c++26")
if is_plat("linux") then
    set_toolchains("gcc")
elseif is_plat("mingw") then
    set_toolchains("mingw")
end

option("with_slint")
    set_default(true)
    set_showmenu(true)
    set_description("Enable the Slint UI")
option_end()

if has_config("with_slint") then
    add_requires("slint v1.17.0", { system = false })
end

option("with_zig")
    -- Default off: the Zig helper currently exports only a placeholder symbol
    -- with no callers. Phase 5 will decide whether it gains a real platform ABI.
    set_default(false)
    set_showmenu(true)
    set_description("Build the optional Zig platform helper target")
option_end()

option("with_simd")
    set_default(false)
    set_showmenu(true)
    set_description("Enable optional assembly/SIMD implementations")
option_end()

option("with_seetaface")
    -- Default on: SeetaFace is the real recognizer backend. A build without it
    -- is a mock-only shell useful only for headless CI; the desktop app is
    -- expected to ship with real face detection.
    set_default(true)
    set_showmenu(true)
    set_description("Enable the SeetaFace recognizer backend")
option_end()

if has_config("with_seetaface") then
    add_requires("seetaface6open", { system = false })
end

local function seetaface_libdir(root)
    local lib64 = path.join(root, "lib64")
    if os.isdir(lib64) then
        return lib64
    end
    local lib = path.join(root, "lib")
    if os.isdir(lib) then
        return lib
    end
end

local function seetaface_runtime_dirs(root)
    local dirs = { }
    for _, dir in ipairs({ path.join(root, "lib64"), path.join(root, "lib") }) do
        if os.isdir(dir) then
            table.insert(dirs, dir)
        end
    end
    return dirs
end

local function seetaface_test_data_dir()
    return path.join(os.projectdir(), "build", "test-data", "seetaface")
end

local function add_seetaface_backend()
    add_packages("seetaface6open", { public = true })
    if is_plat("linux") then
        add_cxxflags("-fopenmp", { force = true, public = true })
        add_ldflags("-fopenmp", { force = true, public = true })
        add_ldflags("-Wl,--disable-new-dtags", { force = true, public = true })
    end
    on_load( function (target)
        local seetaface = target:pkg("seetaface6open")
        if seetaface then
            local root = seetaface:installdir()
            target:add("sysincludedirs", path.join(root, "include"), { public = true })
            for _, dir in ipairs(seetaface_runtime_dirs(root)) do
                target:add("rpathdirs", dir, { public = true })
            end
        end
    end)
end

local function seetaface_root_from_target(target)
    local seetaface = target:pkg("seetaface6open")
    if seetaface then
        return seetaface:installdir()
    end
    return ""
end

local function model_stage_dir()
    return path.join(os.projectdir(), "build", get_config("plat"), get_config("arch"), get_config("mode"), "assets", "models", "seeta")
end

local function apply_cpp_target(kind)
    set_kind(kind)
    add_cxxflags("-Wall", "-Wextra", "-Wpedantic")
    add_includedirs("src", { public = true })
    if is_plat("linux") then
        add_syslinks("pthread", "dl")
    elseif is_plat("windows", "mingw") then
        add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN", "_CRT_SECURE_NO_WARNINGS")
        add_syslinks("ws2_32", "advapi32", "ntdll", "userenv")
    end
end

-- C++ Module interface units: registered per target so non-C++ targets
-- (e.g. su_platform_zig) are not scanned by the module scanner.
-- su_recognizer owns the recognizer + core types modules; su_app and test
-- targets depend on su_recognizer and inherit its module BMIs.

target("su_core")
    set_kind("phony")
    on_build( function ()
        local outdir = path.join(os.projectdir(), "build", get_config("plat"), get_config("arch"), get_config("mode"))
        local manifest = path.join(os.projectdir(), "src", "core-rs", "Cargo.toml")
        local cargo_mode = is_mode("release") and "release" or "debug"
        local cargo_args = {
            "build",
            "--locked",
            "--manifest-path", manifest,
            "--target-dir", path.join(os.projectdir(), "build", "cargo")
        }
        local cargo_target = nil
        if is_plat("mingw") then
            cargo_target = "x86_64-pc-windows-gnu"
            table.insert(cargo_args, "--target")
            table.insert(cargo_args, cargo_target)
        end
        if is_mode("release") then
            table.insert(cargo_args, "--release")
        end
        os.mkdir(outdir)
        os.execv("cargo", cargo_args)
        local cargo_out = cargo_target
            and path.join(os.projectdir(), "build", "cargo", cargo_target, cargo_mode, "libsu_core.a")
            or path.join(os.projectdir(), "build", "cargo", cargo_mode, "libsu_core.a")
        os.cp(cargo_out, path.join(outdir, "libsu_core.a"))
    end)

-- Define the Zig helper only when explicitly enabled: an unconditional
-- toolchain declaration forces a zig install even for default builds.
if has_config("with_zig") then
    target("su_platform_zig")
        set_kind("static")
        set_toolchains("zig")
        set_default(true)
        add_files("src/platform-zig/src/lib.zig")
        before_build( function ()
            local cache_dir = path.join(os.projectdir(), "build", ".zig-cache")
            os.mkdir(cache_dir)
            os.setenv("ZIG_GLOBAL_CACHE_DIR", cache_dir)
        end)
end

target("su_recognizer")
    apply_cpp_target("static")
    add_files("src/recognizer/*.cpp")
    add_files("src/recognizer/image/*.cpp")
    if is_plat("linux") then
        add_files("src/recognizer/camera/v4l2_camera.cpp")
    else
        add_files("src/recognizer/camera/windows_camera.cpp")
        add_files("src/recognizer/camera/windows_mf_camera.cpp")
        add_syslinks("mfplat", "mfreadwrite", "mfuuid", "ole32", "oleaut32")
    end
    add_files("src/modules/su.recognizer.*.cppm")
    add_files("src/modules/su.core.*.cppm")
    add_packages("cimg")
    add_packages("libyuv")
    if is_plat("mingw") then
        add_files("src/platform/windows/print_shim.cpp")
    end
    if has_config("with_seetaface") then
        add_defines("SU_HAS_SEETAFACE=1", { public = true })
        add_defines("SU_SEETAFACE_MODEL_DIR=\"" .. path.unix(model_stage_dir()) .. "\"", { public = true })
        add_seetaface_backend()
        before_build( function ()
            local srcdir = path.join(os.projectdir(), "assets", "models", "seeta")
            local dstdir = path.join(os.projectdir(), "build", get_config("plat"), get_config("arch"), get_config("mode"), "assets", "models", "seeta")
            os.mkdir(dstdir)
            for _, file in ipairs(os.files(path.join(srcdir, "*.csta"))) do
                os.cp(file, dstdir)
            end
        end)
    else
        add_defines("SU_HAS_SEETAFACE=0", { public = true })
    end

    target("su_app")
    apply_cpp_target("binary")
    add_includedirs("src/core-rs/include", {public = true})
    add_packages("nlohmann_json")
    add_deps("su_core", "su_recognizer")
    if is_plat("mingw") then
        -- Ship the Rust Credential Provider DLL next to the GUI binary so a
        -- plain `xmake build` produces the full deployable set.
        add_deps("su_credential_provider")
    end
    if is_plat("linux") then
        add_files("src/app/app_controller.cpp", "src/app/core_bridge.cpp")
    else
        add_files("src/app/windows_app_controller.cpp", "src/app/core_bridge.cpp")
        add_files("src/platform/windows/auth_service/profile_client.cpp")
        add_includedirs("src/platform/windows/auth_service")
    end
    if has_config("with_zig") then
        add_deps("su_platform_zig")
        add_defines("SU_HAS_ZIG_PLATFORM=1")
    else
        add_defines("SU_HAS_ZIG_PLATFORM=0")
    end
    add_files("src/modules/su.recognizer.*.cppm")
    add_files("src/modules/su.core.*.cppm")
    add_files("src/modules/su.app.controller.cppm")
    add_files("src/modules/su.app.user.cppm")
    if not is_plat("linux") then
        add_files("src/platform/windows/user/user_windows.cpp")
    end
    -- Build-time version string for su_app, using the resolved project version.
    add_defines("SU_VERSION_STR=\"" .. _su_version .. "\"")
    if is_plat("linux") then
        add_files("src/modules/su.control.socket.cppm")
        add_files("src/platform/linux/deploy_client/*.cpp")
        add_deps("su_deploy")
    end
    if has_config("with_slint") then
        set_policy("check.target_package_licenses", false)
        add_defines("SU_HAS_SLINT=1")
        add_packages("slint")
        add_files("src/app/slint_main.cpp")
        add_files("src/app/preview_controller.cpp")
        add_files("src/modules/su.app.preview.cppm")
        add_files("src/modules/su.app.session.cppm")
        add_files("src/modules/su.app.i18n.cppm")
        add_files("src/modules/su.app.preferences.cppm")
        add_files("src/modules/su.app.theme.cppm")
        if is_plat("linux") then
            add_syslinks("systemd")
        elseif is_plat("windows", "mingw") then
            -- slint/winit (Windows backend) requires COM/OLE shell + OpenGL APIs
            add_syslinks("ole32", "oleaut32", "shell32", "uuid", "user32", "gdi32", "imm32", "dwmapi", "comdlg32", "version", "opengl32", "ws2_32", "wtsapi32", "wintrust", "crypt32")
            -- GUI subsystem: without -mwindows the PE subsystem is Console and
            -- Windows opens a command-line window alongside the GUI.
            add_ldflags("-mwindows", { force = true })
            -- Embed the application icon into the exe resource section.
            add_files("src/app/su_app.rc")
            -- Windows deployment/integration library (CP registration, auth
            -- service status) used by the GUI deployment panel.
            add_files("src/platform/windows/deploy/deployment.cpp")
            add_includedirs("src/platform/windows/deploy")
        end
        add_files(path.join("build", "generated", "slint", "app_window.cpp"), { always_added = true })
        add_includedirs(path.join("build", "generated", "slint"))
        on_load( function (target)
            local slint = target:pkg("slint")
            if slint then
                target:add("includedirs", path.join(slint:installdir(), "include", "slint"))
                if is_plat("linux") then
                    target:add("rpathdirs", path.join(slint:installdir(), "lib"))
                end
                -- The C++26 module prescan parses every source before any
                -- before_build hook runs; generate the Slint sources at load
                -- time too so clean checkouts do not fail the prescan.
                local compiler = path.join(slint:installdir(), "bin", "slint-compiler")
                local outputdir = path.join(os.projectdir(), "build", "generated", "slint")
                if os.isfile(compiler) and not os.isfile(path.join(outputdir, "app_window.cpp")) then
                    os.mkdir(outputdir)
                    os.execv(compiler, {
                        "-f", "cpp",
                        "--cpp-namespace", "su::app::ui",
                        "-o", path.join(outputdir, "app_window.h"),
                        "--cpp-file", path.join(outputdir, "app_window.cpp"),
                        path.join(os.projectdir(), "src", "app", "ui", "app.slint")
                    })
                end
            end
        end)
        before_build( function (target)
            local slint = assert(target:pkg("slint"), "slint package is required when with_slint=y")
            local compiler = path.join(slint:installdir(), "bin", "slint-compiler")
            local outputdir = path.join(os.projectdir(), "build", "generated", "slint")
            os.mkdir(outputdir)
            os.execv(compiler, {
                "-f", "cpp",
                "--cpp-namespace", "su::app::ui",
                "-o", path.join(outputdir, "app_window.h"),
                "--cpp-file", path.join(outputdir, "app_window.cpp"),
                path.join(os.projectdir(), "src", "app", "ui", "app.slint")
            })
        end)
        after_build( function (target)
            local outputdir = path.join(target:targetdir(), "assets", "i18n")
            os.rm(outputdir)
            os.mkdir(outputdir)
            for _, file in ipairs(os.files(path.join(os.projectdir(), "assets", "i18n", "*.json"))) do
                os.cp(file, outputdir)
            end
            -- Ship the .ico next to the executable; the GUI applies it to the
            -- native window (title bar / taskbar) at startup via WM_SETICON.
            if is_plat("windows", "mingw") then
                os.cp(path.join(os.projectdir(), "assets", "icons", "Smile2Unlock.ico"), target:targetdir())
            end
        end)
    else
        add_defines("SU_HAS_SLINT=0")
        add_files("src/app/console_main.cpp")
    end
    add_linkdirs(path.join(os.projectdir(), "build", get_config("plat"), get_config("arch"), get_config("mode")))
    add_links("su_core")

if is_plat("linux") then
    target("su_deploy")
        apply_cpp_target("static")
        add_files("src/platform/linux/deploy/*.cpp")
        add_headerfiles("src/platform/linux/deploy/*.h")
        add_packages("nlohmann_json", { public = true })
        add_defines("SU_VERSION_STR=\"" .. _su_version .. "\"")

    target("su_deploy_helper")
        apply_cpp_target("binary")
        add_files("src/platform/linux/deploy_helper/*.cpp")
        add_files("src/modules/su.control.socket.cppm")
        add_deps("su_deploy")
        add_packages("nlohmann_json")
        add_syslinks("systemd")
        add_tests("version", {runargs = {"--version"}})

elseif is_plat("windows", "mingw") then
    -- Windows counterpart of su_deploy_helper: runs elevated (UAC) to
    -- perform credential-provider registration and auth-service actions
    -- triggered from the GUI deployment panel.
    target("su_deploy_helper")
        apply_cpp_target("binary")
        add_files("src/platform/windows/deploy_helper/main.cpp")
        add_files("src/platform/windows/deploy_helper/helper.rc")
        add_files("src/platform/windows/deploy/deployment.cpp")
        add_includedirs("src/platform/windows/deploy")
        add_packages("nlohmann_json")
        add_syslinks("advapi32", "user32", "shell32", "ole32", "uuid", "wintrust", "crypt32")
        add_tests("version", {runargs = {"--version"}})
end

if is_plat("linux") then
    target("pam_smile2unlock")
        apply_cpp_target("shared")
        set_filename("pam_smile2unlock.so")
        set_prefixname("")
        add_files("src/platform/linux/pam/*.cpp")
        add_files("src/modules/su.control.socket.cppm")
        add_packages("nlohmann_json")
        add_headerfiles("src/platform/linux/pam/*.h")
        add_syslinks("pam")

    target("su_authd")
        apply_cpp_target("binary")
        add_files("src/platform/linux/authd/*.cpp", "src/app/core_bridge.cpp")
        add_files(
            "src/modules/su.auth.daemon.cppm",
            "src/modules/su.auth.storage.cppm",
            "src/modules/su.auth.user.cppm",
            "src/modules/su.control.socket.cppm")
        add_files("src/modules/su.core.*.cppm", "src/modules/su.recognizer.*.cppm")
        add_includedirs("src/core-rs/include")
        add_packages("nlohmann_json")
        add_deps("su_core", "su_recognizer")
        add_rpathdirs("/usr/lib/smile2unlock")
        add_linkdirs(path.join(os.projectdir(), "build", get_config("plat"), get_config("arch"), get_config("mode")))
        add_links("su_core")

    target("su_control_socket_smoke_test")
        apply_cpp_target("binary")
        add_files("tests/control/*.cpp", "src/modules/su.control.socket.cppm")
        add_packages("nlohmann_json")
        add_tests("default")

    target("su_session_lock_monitor_smoke_test")
        apply_cpp_target("binary")
        add_files("tests/session/*.cpp", "src/modules/su.app.session.cppm")
        add_syslinks("systemd")
        add_tests("default")

    target("su_deploy_test")
        apply_cpp_target("binary")
        add_files("tests/deploy/*.cpp")
        add_deps("su_deploy")
        add_tests("default")

    target("su_multi_user_auth_test")
        apply_cpp_target("binary")
        add_files(
            "tests/multi_user/*.cpp",
            "src/modules/su.auth.user.cppm",
            "src/modules/su.app.user.cppm",
            "src/modules/su.core.types.cppm")
        if not is_plat("linux") then
            add_files("src/platform/windows/user/user_windows.cpp")
        end
        add_tests("default")

    target("su_pam_integration_test")
        apply_cpp_target("binary")
        add_files("tests/pam/*.cpp", "src/modules/su.control.socket.cppm")
        add_deps("pam_smile2unlock")
        add_packages("nlohmann_json")
        add_defines("SU_PAM_MODULE_PATH=\"" .. path.unix(path.join(
            os.projectdir(), "build", get_config("plat"), get_config("arch"),
            get_config("mode"), "pam_smile2unlock.so")) .. "\"")
        add_syslinks("pam")
        add_tests("default")

    target("su_key_provider_test")
        apply_cpp_target("binary")
        add_files("tests/storage/*.cpp")
        add_files("src/modules/su.auth.storage.cppm", "src/modules/su.core.types.cppm")
        add_tests("default")

    target("su_pam_acceptance")
        apply_cpp_target("binary")
        add_files("src/platform/linux/pam_acceptance/*.cpp")
        add_files("src/modules/su.control.socket.cppm")
        add_deps("pam_smile2unlock")
        add_packages("nlohmann_json")
        add_defines("SU_PAM_MODULE_PATH=\"" .. path.unix(path.join(
            os.projectdir(), "build", get_config("plat"), get_config("arch"),
            get_config("mode"), "pam_smile2unlock.so")) .. "\"")
        add_syslinks("pam")
        add_tests("help", {
            runargs = {"--help"}
        })
end

if is_plat("windows", "mingw") then
    target("su_windows_storage")
        apply_cpp_target("static")
        set_default(false)
        set_toolchains("mingw")
        add_files("src/platform/windows/security/*.cpp")
        add_headerfiles("src/platform/windows/security/*.h")
        add_syslinks("ncrypt", "bcrypt", "crypt32", "shell32", "ole32")

    -- LocalSystem auth service: named-pipe host for the logon-secret store,
    -- used by the C++ Credential Provider baseline and the Rust CP client.
    target("su_auth_service")
        apply_cpp_target("binary")
        set_targetdir("$(builddir)/$(plat)/$(arch)/$(mode)")
        add_files("src/platform/windows/security/storage_key_provider.cpp")
        add_files("src/platform/windows/security/logon_secret_store.cpp")
        add_files("src/platform/windows/security/face_profile_store.cpp")
        add_files("src/platform/windows/auth_service/logon_secret_server.cpp")
        add_files("src/platform/windows/auth_service/service_main.cpp")
        add_includedirs(
            "src/platform/windows/security",
            "src/platform/windows/auth_service",
            "src/core-rs/include")
        add_linkdirs(path.join(os.projectdir(), "build", get_config("plat"), get_config("arch"), get_config("mode")))
        add_links("su_core")
        add_ldflags("-static", "-municode", "-mwindows", {force = true})
        add_syslinks(
            "ncrypt", "bcrypt", "crypt32", "shell32", "ole32", "advapi32",
            "userenv", "wtsapi32", "ntdll", "ws2_32", "uuid")
        add_deps("su_core")

    -- Minimal camera worker launched by the LocalSystem broker in the target
    -- console session. It has no UI and only returns liveness + embedding
    -- evidence through inherited anonymous-pipe handles.
    target("su_recognition_agent")
        apply_cpp_target("binary")
        add_files("src/platform/windows/recognition_agent/main.cpp")
        add_files("src/modules/su.recognizer.*.cppm")
        add_deps("su_recognizer")
        add_ldflags("-municode", "-mwindows", {force = true})
        add_syslinks("mfplat", "mfreadwrite", "mfuuid", "ole32", "oleaut32")

    -- Interactive per-user password enrollment/clear utility. The service
    -- validates the caller SID before accepting either operation.
    target("su_password_tool")
        apply_cpp_target("binary")
        add_files("src/platform/windows/password_tool/main.cpp")
        add_includedirs("src/platform/windows/auth_service")
        add_ldflags("-static", "-municode", {force = true})
        add_syslinks("advapi32", "userenv")
end

target("su_face_auth_smoke_test")
    apply_cpp_target("binary")
    add_files("tests/face_auth/*.cpp")
    add_deps("su_core", "su_recognizer")
    add_files("src/app/core_bridge.cpp")
    add_files("src/modules/su.core.*.cppm")
    add_files("src/modules/su.recognizer.*.cppm")
    add_includedirs("src/core-rs/include")
    add_linkdirs(path.join(os.projectdir(), "build", get_config("plat"), get_config("arch"), get_config("mode")))
    add_links("su_core")
    if not is_plat("mingw") then
        add_tests("default")
    end

target("su_theme_test")
    apply_cpp_target("binary")
    add_files(
        "tests/theme/*.cpp",
        "src/modules/su.app.preferences.cppm",
        "src/modules/su.app.theme.cppm")
    add_packages("nlohmann_json")
    if not is_plat("mingw") then
        add_tests("default")
    end

target("su_windows_sid_rate_limiter_test")
    apply_cpp_target("binary")
    add_files("tests/windows/sid_rate_limiter.cpp")
    add_includedirs("src/platform/windows/auth_service")
    if is_plat("mingw") then
        add_ldflags("-static", {force = true})
    end
    add_tests("default")

-- A real (binary) target whose test runs the Rust core unit suite via Cargo.
-- Using a binary target instead of a phony one because xmake's on_test only
-- reliably reports pass/fail for targets with a build artifact. The binary is
-- a trivial main that is never run; cargo test is what on_test executes.
target("su_core_rust_tests")
    apply_cpp_target("binary")
    add_files("tests/rust/main.cpp")
    on_test(function (target)
        local ok = os.execv("cargo", {
            "test",
            "--locked",
            "--manifest-path",
            path.join(os.projectdir(), "src", "core-rs", "Cargo.toml"),
        }, { try = true })
        if ok == nil or ok == false or (type(ok) == "number" and ok ~= 0) then
            os.raise("cargo test failed: " .. tostring(ok))
        end
        return true
    end)
    add_tests("default")

target("su_credential_provider")
    -- Rust cdylib Credential Provider, cross-built from the Linux host.
    -- Mirrors su_core's cargo integration: only active for mingw (the crate
    -- is cfg(windows)-only); a phony target so `xmake build` produces the
    -- DLL next to su_app.exe without a manual cargo invocation.
    set_kind("phony")
    on_build( function ()
        if not is_plat("mingw") then
            return
        end
        local outdir = path.join(os.projectdir(), "build", get_config("plat"), get_config("arch"), get_config("mode"))
        local manifest = path.join(os.projectdir(), "src", "platform", "windows", "credential_provider_rs", "Cargo.toml")
        local cargo_mode = is_mode("release") and "release" or "debug"
        local cargo_args = {
            "build",
            "--locked",
            "--manifest-path", manifest,
            "--target", "x86_64-pc-windows-gnu",
            "--target-dir", path.join(os.projectdir(), "build", "cargo", "credential_provider_rs")
        }
        if is_mode("release") then
            table.insert(cargo_args, "--release")
        end
        os.mkdir(outdir)
        os.execv("cargo", cargo_args)
        local cargo_out = path.join(
            os.projectdir(), "build", "cargo", "credential_provider_rs",
            "x86_64-pc-windows-gnu", cargo_mode, "su_credential_provider.dll")
        os.cp(cargo_out, path.join(outdir, "su_credential_provider.dll"))
    end)

target("su_credential_provider_rust_tests")
    apply_cpp_target("binary")
    add_files("tests/rust/main.cpp")
    on_test(function (target)
        -- The crate is Windows-only (cfg(windows) modules, windows crate
        -- dependency); native cargo test cannot compile it on Linux. The
        -- mingw build runs the COM/IPC suite under wine via the crate-local
        -- .cargo/config.toml runner.
        if not is_plat("mingw") then
            return true
        end
        local args = {
            "test",
            "--locked",
            "--manifest-path",
            path.join(os.projectdir(), "src", "platform", "windows", "credential_provider_rs", "Cargo.toml"),
            "--target",
            "x86_64-pc-windows-gnu",
        }
        local ok = os.execv("cargo", args, { try = true })
        if ok == nil or ok == false or (type(ok) == "number" and ok ~= 0) then
            os.raise("credential provider cargo test failed: " .. tostring(ok))
        end
        return true
    end)
    add_tests("default")

if has_config("with_seetaface") then
    target("su_seetaface_pipeline_smoke_test")
        apply_cpp_target("binary")
        add_files("tests/seetaface/*.cpp")
        add_deps("su_core", "su_recognizer")
        add_packages("seetaface6open")
        add_files("src/app/core_bridge.cpp")
        add_files("src/modules/su.core.*.cppm")
        add_files("src/modules/su.recognizer.*.cppm")
        add_includedirs("src/core-rs/include")
        add_defines("SU_SEETAFACE_TEST_DATA_DIR=\"" .. path.unix(seetaface_test_data_dir()) .. "\"")
        add_linkdirs(path.join(os.projectdir(), "build", get_config("plat"), get_config("arch"), get_config("mode")))
        add_links("su_core")
        on_load( function (target)
            local root = seetaface_root_from_target(target)
            target:add("sysincludedirs", path.join(root, "include"))
            for _, dir in ipairs(seetaface_runtime_dirs(root)) do
                target:add("rpathdirs", dir)
            end
        end)
        before_build( function (target)
            local root = seetaface_root_from_target(target)
            local recognizer_samples = path.join(root, "src", "FaceRecognizer6", "example")
            local fas_samples = path.join(root, "src", "FaceAntiSpoofingX6", "example")
            local dstdir = seetaface_test_data_dir()
            os.mkdir(dstdir)
            os.execv("magick", {
                path.join(recognizer_samples, "1.png"),
                "-colorspace", "RGB",
                "-alpha", "off",
                path.join(dstdir, "official_face_1.ppm")
            })
            os.execv("magick", {
                path.join(fas_samples, "hu.ge.jpg"),
                "-colorspace", "RGB",
                path.join(dstdir, "official_face_2.ppm")
            })
            -- Stage the original PNG/JPG too, so the CImg image-loader path
            -- (RecognizerService::extract_from_image) can be exercised against
            -- real encoded files, not only pre-converted PPM fixtures.
            os.cp(path.join(recognizer_samples, "1.png"), path.join(dstdir, "official_face_1.png"))
            os.cp(path.join(fas_samples, "hu.ge.jpg"), path.join(dstdir, "official_face_2.jpg"))
        end)
        if not is_plat("mingw") then
            add_tests("default")
        end
end

-- Auto-generate compile_commands.json for clangd LSP after each full build.
-- Only triggers once (on the su_app target, which is the last C++ target).
-- The --lsp=clangd flag produces clangd-compatible output with module info.
local cdb_generated = false
after_build(function (target)
    if not cdb_generated and target:name() == "su_app" then
        cdb_generated = true
        os.exec("xmake project -k compile_commands --lsp=clangd 2>/dev/null || true")
    end
end)
