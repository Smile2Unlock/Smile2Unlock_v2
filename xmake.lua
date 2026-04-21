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
add_requires("eui")
add_requires("seetaface6open", {system = false})
add_requires("papiliocharontis", {system = false})
add_requires("reflect-cpp")

set_encodings("utf-8")
set_languages("c++26")
set_toolchains("clang")

local function apply_cpp26_target(kind)
    set_kind(kind)
    set_policy("build.c++.modules", true)
    set_policy("build.c++.modules.std", true)
    set_warnings("all")
    add_includedirs("src/rewrite/include", {public = true})
    add_packages("tbox", "seetaface6open")
    if is_plat("linux") then
        add_cxflags("-fPIC", {force = true})
        add_syslinks("pthread", "dl")
    end
end
