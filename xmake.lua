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

option("with_seetaface")
    set_default(false)
    set_showmenu(true)
    set_description("Enable the optional SeetaFace recognizer backend")
option_end()

local seetaface_links = {
    "SeetaFaceAntiSpoofingX600",
    "SeetaFaceDetector600",
    "SeetaFaceLandmarker600",
    "SeetaFaceRecognizer610",
    "SeetaAuthorize",
    "tennis"
}

local function has_seetaface_libraries(libdir)
    if not libdir or not os.isdir(libdir) then
        return false
    end
    for _, link in ipairs(seetaface_links) do
        if not os.isfile(path.join(libdir, "lib" .. link .. ".so"))
            and not os.isfile(path.join(libdir, "lib" .. link .. ".a"))
            and not os.isfile(path.join(libdir, link .. ".lib")) then
            return false
        end
    end
    return true
end

local function seetaface_libdir(root)
    local lib64 = path.join(root, "lib64")
    if has_seetaface_libraries(lib64) then
        return lib64
    end
    local lib = path.join(root, "lib")
    if has_seetaface_libraries(lib) then
        return lib
    end
end

local function seetaface_runtime_dirs(root)
    local dirs = {}
    for _, dir in ipairs({path.join(root, "lib64"), path.join(root, "lib")}) do
        if os.isdir(dir) then
            table.insert(dirs, dir)
        end
    end
    return dirs
end

local function find_seetaface_root()
    local candidates = {}
    local explicit_root = os.getenv("SU_SEETAFACE_ROOT")
    if explicit_root and explicit_root ~= "" then
        table.insert(candidates, explicit_root)
    end
    local home = os.getenv("HOME")
    if home then
        for _, candidate in ipairs(os.dirs(path.join(home, ".xmake", "packages", "s", "seetaface6open", "latest", "*"))) do
            table.insert(candidates, candidate)
        end
    end

    for _, candidate in ipairs(candidates) do
        local include_marker = path.join(candidate, "include", "seeta", "FaceRecognizer.h")
        local libdir = seetaface_libdir(candidate)
        if os.isfile(include_marker) and libdir then
            return candidate, libdir
        end
    end
end

local function add_seetaface_backend()
    local root, libdir = find_seetaface_root()
    if not root or not libdir then
        raise("SeetaFace6Open not found; set SU_SEETAFACE_ROOT or install local-repo package seetaface6open")
    end
    add_includedirs(path.join(root, "include"), {public = true})
    for _, dir in ipairs(seetaface_runtime_dirs(root)) do
        add_linkdirs(dir, {public = true})
    end
    for _, link in ipairs(seetaface_links) do
        add_links(link, {public = true})
    end
    if is_plat("linux") then
        add_cxxflags("-fopenmp", {force = true, public = true})
        add_ldflags("-fopenmp", {force = true, public = true})
        add_ldflags("-Wl,--disable-new-dtags", {force = true, public = true})
        for _, dir in ipairs(seetaface_runtime_dirs(root)) do
            add_rpathdirs(dir, {public = true})
        end
    end
end

local function model_stage_dir()
    return path.join(os.projectdir(), "build", get_config("plat"), get_config("arch"), get_config("mode"), "assets", "models", "seta")
end

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
        local manifest = path.join(os.projectdir(), "src", "core-rs", "Cargo.toml")
        local cargo_mode = is_mode("release") and "release" or "debug"
        local cargo_args = {
            "build",
            "--manifest-path", manifest,
            "--target-dir", path.join(os.projectdir(), "build", "cargo")
        }
        if is_mode("release") then
            table.insert(cargo_args, "--release")
        end
        os.mkdir(outdir)
        os.execv("cargo", cargo_args)
        os.cp(path.join(os.projectdir(), "build", "cargo", cargo_mode, "libsu_core.a"), path.join(outdir, "libsu_core.a"))
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
    if has_config("with_seetaface") then
        add_defines("SU_HAS_SEETAFACE=1", {public = true})
        add_defines("SU_SEETAFACE_MODEL_DIR=\"" .. path.unix(model_stage_dir()) .. "\"", {public = true})
        add_seetaface_backend()
        before_build(function ()
            local srcdir = path.join(os.projectdir(), "FaceRecognizer", "resources", "models")
            local dstdir = path.join(os.projectdir(), "build", get_config("plat"), get_config("arch"), get_config("mode"), "assets", "models", "seta")
            os.mkdir(dstdir)
            for _, file in ipairs(os.files(path.join(srcdir, "*.csta"))) do
                os.cp(file, dstdir)
            end
        end)
    else
        add_defines("SU_HAS_SEETAFACE=0", {public = true})
    end

target("su_app")
    apply_cpp_target("binary")
    add_files("src/app/app_controller.cpp", "src/app/core_bridge.cpp")
    add_headerfiles("src/app/*.h")
    add_includedirs("src/core-rs/include", {public = true})
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
    add_deps("su_core", "su_recognizer")
    add_files("src/app/core_bridge.cpp")
    add_includedirs("src/core-rs/include")
    add_linkdirs(path.join(os.projectdir(), "build", get_config("plat"), get_config("arch"), get_config("mode")))
    add_links("su_core")
    add_tests("default")
