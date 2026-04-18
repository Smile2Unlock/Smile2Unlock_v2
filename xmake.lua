add_rules("mode.debug", "mode.release")
add_rules("plugin.compile_commands.autoupdate", {outputdir = ".vscode"})

add_repositories("myrepo local-repo")

add_requires("tbox", {
    configs = {
        sqlite3 = false,
        mbedtls = false,
        database = false,
        coroutine = true,
        hash = true,
        shared = false,
        cflags = "-Wno-error=uninitialized-const-pointer",
        cxflags = "-Wno-error=uninitialized-const-pointer"
    }
})
add_requires("glfw", "imgui", {configs = {glfw = true, opengl3 = true}})

set_encodings("utf-8")
set_languages("c++26")
set_toolchains("clang")

local function apply_cpp26_target(kind)
    set_kind(kind)
    set_policy("build.c++.modules", true)
    set_policy("build.c++.modules.std", true)
    set_warnings("all")
    add_includedirs("src/rewrite/include", {public = true})
    add_packages("tbox")
    if is_plat("linux") then
        add_cxflags("-fPIC", {force = true})
        add_syslinks("pthread", "dl")
    end
end

target("su_backend_core")
    apply_cpp26_target("static")
    add_files("src/rewrite/modules/**.cppm")
    add_files("src/rewrite/backend/backend_core.cpp")
    add_headerfiles("src/rewrite/include/(rewrite/**.hpp)")

target("su_backendd")
    apply_cpp26_target("binary")
    add_deps("su_backend_core", {public = true})
    add_files("src/rewrite/backendd/main.cpp")

target("su_gui")
    apply_cpp26_target("binary")
    add_deps("su_backend_core", {public = true})
    add_packages("glfw", "imgui")
    add_files("src/rewrite/gui/main.cpp")
    if is_plat("linux") then
        add_syslinks("GL", "X11", "Xi", "Xrandr", "Xxf86vm", "Xinerama", "Xcursor")
    elseif is_plat("windows", "mingw") then
        add_syslinks("opengl32", "gdi32", "user32", "shell32")
    end

target("su_facerecognizer")
    apply_cpp26_target("binary")
    add_deps("su_backend_core", {public = true})
    add_files("src/rewrite/facerecognizer/main.cpp")

if is_plat("linux") then
    target("pam_smile2unlock")
        apply_cpp26_target("shared")
        set_filename("pam_smile2unlock.so")
        set_prefixname("")
        add_deps("su_backend_core", {public = true})
        add_files("src/rewrite/pam/pam_smile2unlock.cpp")
        add_syslinks("pam")
end

if is_plat("windows", "mingw") then
    target("SampleV2CredentialProvider")
        apply_cpp26_target("shared")
        set_filename("SampleV2CredentialProvider.dll")
        set_prefixname("")
        add_deps("su_backend_core", {public = true})
        add_files("src/rewrite/windows/credential_provider_placeholder.cpp")
        add_defines("UNICODE", "_UNICODE")
        add_syslinks("user32")
end
