add_rules("mode.debug", "mode.release")
add_rules("plugin.compile_commands.autoupdate", {outputdir = ".vscode"})

add_repositories("myrepo local-repo")

add_requires("tbox", {
    configs = {
        sqlite3 = true,
        mbedtls = false,
        database = true,
        coroutine = true,
        hash = true,
        shared = false,
        cflags = "-Wno-error=uninitialized-const-pointer",
        cxflags = "-Wno-error=uninitialized-const-pointer"
    },
    system = false
})
add_requires("mbedtls", {configs = {shared = true}, system = false})
add_requires("libyuv")
add_requires("glad")
add_requires("glfw")
add_requires("curl")
add_requires("eui")
add_requires("seetaface6open", {system = false})
add_requires("papiliocharontis", {system = false})
add_requires("reflect-cpp")

set_encodings("utf-8")
set_languages("c++26")
set_toolchains("clang")
set_runtimes("c++_shared")
add_cxxflags("-stdlib=libc++", {force = true})
add_ldflags("-stdlib=libc++", {force = true})
add_shflags("-stdlib=libc++", {force = true})

local function apply_cpp26_target(kind)
    set_kind(kind)
    set_policy("build.c++.modules", true)
    set_warnings("all")
    add_includedirs("src/include", {public = true})
    add_packages("tbox", "papiliocharontis", "reflect-cpp")
    if is_plat("linux") then
        add_cxflags("-fPIC", {force = true})
        add_syslinks("pthread", "dl")
    end
end

-- su_backend_core: static library for platform-agnostic business logic
target("su_backend_core")
    apply_cpp26_target("static")
    add_files("src/core/**.cpp")

-- su_app: main desktop host with EUI GUI
target("su_app")
    apply_cpp26_target("binary")
    add_files("src/app/**.cpp")
    add_packages("eui", "glad", "glfw", "curl")
    add_links("curl")
    if is_plat("linux") then
        add_syslinks("GL", "X11")
    elseif is_plat("windows") then
        add_syslinks("opengl32")
    end
    after_build(function (target)
        if os.isfile("tools/sync_clangd_modules.py") then
            os.vrunv("python3", {"tools/sync_clangd_modules.py"})
        end
    end)
