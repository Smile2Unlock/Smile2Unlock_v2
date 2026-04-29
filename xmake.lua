add_rules("mode.debug", "mode.release")
-- add_rules("plugin.compile_commands.autoupdate", {outputdir = ".vscode", lsp = "clangd"})

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

local kSeetaFaceInstallDir = "/home/ation_ciger/.xmake/packages/s/seetaface6open/latest/f139fbca2e3243b5a44b948e5a249061"
local kStageModelsScript = path.absolute("scripts/stage_models.py")

local function apply_cpp26_target(kind)
    set_kind(kind)
    set_policy("build.c++.modules", true)
    set_policy("build.c++.modules.std", true)
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

target("su_facerecognizer")
    apply_cpp26_target("binary")
    add_files(
        "src/facerecognizer/main.cpp",
        "src/facerecognizer/seetaface.cpp",
        "src/facerecognizer/modules/**.cppm"
    )
    add_packages("libyuv", "seetaface6open")
    add_linkdirs("/home/ation_ciger/.xmake/packages/s/seetaface6open/latest/f139fbca2e3243b5a44b948e5a249061/lib64")
    add_links(
        "SeetaFaceAntiSpoofingX600",
        "SeetaFaceDetector600",
        "SeetaFaceLandmarker600",
        "SeetaFaceRecognizer610",
        "SeetaAuthorize",
        "tennis"
    )
    if is_plat("linux") then
        add_rpathdirs("$ORIGIN")
        add_syslinks("m")
    elseif is_plat("windows") then
        add_syslinks("ws2_32")
    end
    after_build(function (target)
        if is_plat("linux") then
            local seeta_libdir = path.join(kSeetaFaceInstallDir, "lib64")
            if os.isdir(seeta_libdir) then
                os.cp(path.join(seeta_libdir, "*.so"), target:targetdir())
                os.cp(path.join(seeta_libdir, "*.so.*"), target:targetdir())
            end
        elseif is_plat("windows") then
            local seeta_bindir = path.join(kSeetaFaceInstallDir, "bin")
            if os.isdir(seeta_bindir) then
                os.cp(path.join(seeta_bindir, "*.dll"), target:targetdir())
            end
        end

        local models_dir = path.absolute("assets/models")
        if os.isdir(models_dir) then
            local python_program = os.isexec("python3") and "python3" or "python"
            os.execv(python_program, {
                kStageModelsScript,
                "--models-dir", models_dir,
                "--destination", path.join(target:targetdir(), "assets", "models")
            })
        end
    end)

-- su_app: main desktop host with EUI GUI
target("su_app")
    apply_cpp26_target("binary")
    add_files("src/app/**.cpp", "src/app/**.cppm")
    -- add_files("src/app/i18n/**.cppm",
    --     "src/app/pages/**.cppm",
    --     "src/app/widgets/**.cppm",
    --     "src/app/ui/**.cppm",
    --     "src/app/presenters/**.cppm",
    --     "src/app/runtime/**.cppm")
    add_packages("eui", "glad", "glfw", "curl")
    add_links("curl")
    if is_plat("linux") then
        add_syslinks("GL", "X11")
    elseif is_plat("windows") then
        add_syslinks("opengl32")
    end
    after_build(function (target)
        if os.isdir("assets") then
            local target_assets_dir = path.join(target:targetdir(), "assets")
            os.mkdir(target_assets_dir)
            for _, asset_name in ipairs({"i18n", "icons"}) do
                local source_dir = path.join("assets", asset_name)
                if os.isdir(source_dir) then
                    os.cp(source_dir, target_assets_dir)
                end
            end
            local models_dir = path.absolute("assets/models")
            if os.isdir(models_dir) then
                local python_program = os.isexec("python3") and "python3" or "python"
                os.execv(python_program, {
                    kStageModelsScript,
                    "--models-dir", models_dir,
                    "--destination", path.join(target_assets_dir, "models")
                })
            end
        end
        for _, pkg in ipairs(target:orderpkgs()) do
            if pkg:name() == "eui" then
                local fontdir = path.join(pkg:installdir(), "include", "eui", "font")
                if os.isdir(fontdir) then
                    os.cp(fontdir, path.join(target:targetdir(), "font"))
                end
            end
        end
    end)
