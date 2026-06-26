add_rules("mode.debug", "mode.release")

add_repositories("local-repo local-repo")

add_requires("slint v1.17.0", {system = false, optional = true})

set_encodings("utf-8")
set_languages("c++26")
set_toolchains("gcc")

option("with_slint")
    set_default(true)
    set_showmenu(true)
    set_description("Enable the Slint UI")
option_end()

option("with_zig")
    set_default(true)
    set_showmenu(true)
    set_description("Build the optional Zig platform helper target")
option_end()

option("with_simd")
    set_default(false)
    set_showmenu(true)
    set_description("Enable optional assembly/SIMD implementations")
option_end()

local function apply_cpp_target(kind)
    set_kind(kind)
    add_cxxflags("-Wall", "-Wextra", "-Wpedantic")
    add_includedirs("src", {public = true})
    if is_plat("linux") then
        add_syslinks("pthread", "dl")
    elseif is_plat("windows", "mingw") then
        add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN", "_CRT_SECURE_NO_WARNINGS")
        add_syslinks("ws2_32", "advapi32")
    end
end

target("su_core")
    set_kind("phony")
    on_build(function ()
        local outdir = path.join(os.projectdir(), "build", get_config("plat"), get_config("arch"), get_config("mode"))
        os.mkdir(outdir)
        os.execv("rustc", {
            "--edition=2024",
            "--crate-type=staticlib",
            "-C", "debuginfo=2",
            "-C", "opt-level=0",
            "-o", path.join(outdir, "libsu_core.a"),
            path.join(os.projectdir(), "src", "core-rs", "src", "lib.rs")
        })
    end)

target("su_platform_zig")
    set_kind("static")
    set_toolchains("zig")
    set_default(has_config("with_zig"))
    add_files("src/platform-zig/src/lib.zig")
    before_build(function ()
        local cache_dir = path.join(os.projectdir(), "build", ".zig-cache")
        os.mkdir(cache_dir)
        os.setenv("ZIG_GLOBAL_CACHE_DIR", cache_dir)
    end)

target("su_recognizer")
    apply_cpp_target("static")
    add_files("src/recognizer/*.cpp")
    add_headerfiles("src/recognizer/*.h")

target("su_app")
    apply_cpp_target("binary")
    add_files("src/app/app_controller.cpp", "src/app/core_bridge.cpp")
    add_headerfiles("src/app/*.h")
    add_deps("su_core", "su_recognizer")
    if has_config("with_zig") then
        add_deps("su_platform_zig")
        add_defines("SU_HAS_ZIG_PLATFORM=1")
    else
        add_defines("SU_HAS_ZIG_PLATFORM=0")
    end
    if has_config("with_slint") then
        add_defines("SU_HAS_SLINT=1")
        add_packages("slint")
        add_files("src/app/slint_main.cpp")
        add_files(path.join("build", "generated", "slint", "app_window.cpp"), {always_added = true})
        add_includedirs(path.join("build", "generated", "slint"))
        on_load(function (target)
            local slint = target:pkg("slint")
            if slint then
                target:add("includedirs", path.join(slint:installdir(), "include", "slint"))
                if is_plat("linux") then
                    target:add("rpathdirs", path.join(slint:installdir(), "lib"))
                end
            end
        end)
        before_build(function (target)
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
    else
        add_defines("SU_HAS_SLINT=0")
        add_files("src/app/console_main.cpp")
    end
    add_linkdirs(path.join(os.projectdir(), "build", get_config("plat"), get_config("arch"), get_config("mode")))
    add_links("su_core")

target("pam_smile2unlock")
    apply_cpp_target("shared")
    set_filename("pam_smile2unlock.so")
    set_prefixname("")
    add_files("src/platform/linux/pam/*.cpp")
    add_headerfiles("src/platform/linux/pam/*.h")
    if is_plat("linux") then
        add_syslinks("pam")
    end

target("su_protocol_smoke_test")
    apply_cpp_target("binary")
    add_files("tests/protocol/*.cpp")
    add_deps("su_core")
    add_files("src/app/core_bridge.cpp")
    add_linkdirs(path.join(os.projectdir(), "build", get_config("plat"), get_config("arch"), get_config("mode")))
    add_links("su_core")
    add_tests("default")
