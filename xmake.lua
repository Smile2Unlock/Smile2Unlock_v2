add_rules("mode.debug", "mode.release")
add_rules("plugin.compile_commands.autoupdate", {outputdir = ".vscode"})

add_repositories("myrepo local-repo")
add_requires("seetaface6open")
add_requires("cxxopts")
add_requires("boost", {configs = {asio = true}})
add_requires("libyuv")
add_requires("glfw", "imgui", {configs = {glfw = true, opengl3 = true}})
add_requires("sqlite3")
add_requires("mbedtls")

set_encodings("utf-8")
set_languages("c++26")
set_plat("mingw")
set_toolchains("clang")
set_runtimes("c++_static")

local function apply_common_windows_settings(winver)
    if is_plat("windows", "mingw") then
        add_defines("_WIN32_WINNT=" .. winver, "NOMINMAX", "_CRT_SECURE_NO_WARNINGS", "WIN32_LEAN_AND_MEAN")
        add_links("Advapi32", "mfplat", "mf", "mfreadwrite", "mfuuid", "ole32", "uuid")
    end
end

local function apply_common_module_binary_target()
    set_kind("binary")
    set_policy("build.c++.modules", true)
end

local app_version_tag = "v0.0.0"
local releases_api_url = "https://api.github.com/repos/Smile2Unlock/Smile2Unlock_v2/releases/latest"
local releases_page_url = "https://github.com/Smile2Unlock/Smile2Unlock_v2/releases"

local function find_llvm_runtime_dll_paths(dll_names)
    if not is_plat("windows", "mingw") then
        return {}
    end

    local search_dirs = {}
    local seen = {}
    local resolved = {}

    local function add_search_dir(dir)
        if dir and os.isdir(dir) and not seen[dir] then
            table.insert(search_dirs, dir)
            seen[dir] = true
        end
    end

    local mingw = get_config("mingw")
    if mingw then
        add_search_dir(path.join(mingw, "bin"))
    end

    local env_path = os.getenv("PATH")
    if env_path then
        for dir in env_path:gmatch("([^;]+)") do
            add_search_dir(dir)
        end
    end

    for _, dll_name in ipairs(dll_names) do
        for _, dir in ipairs(search_dirs) do
            local dll_path = path.join(dir, dll_name)
            if os.isfile(dll_path) then
                resolved[dll_name] = dll_path
                break
            end
        end
    end

    return resolved
end

local function copy_llvm_runtime_dlls(batchcmds, target, dll_names)
    local resolved = find_llvm_runtime_dll_paths(dll_names)
    for _, dll_name in ipairs(dll_names) do
        local dll_path = resolved[dll_name]
        if dll_path then
            batchcmds:cp(dll_path, target:targetdir())
        else
            print("警告: 找不到 LLVM 运行时 DLL: " .. dll_name)
        end
    end
end

target("FaceRecognizer")
    apply_common_module_binary_target()
    add_files("FaceRecognizer/src/modules/*.cppm")
    add_files("common/modules/*.cppm")
    add_files("FaceRecognizer/src/*.cpp")
    add_headerfiles("FaceRecognizer/src/**.h")
    add_headerfiles("FaceRecognizer/src/**.hpp")
    add_includedirs("FaceRecognizer/src", {public = false})
    add_includedirs("common", {public = false})
    apply_common_windows_settings("0x0A00")

    add_packages("seetaface6open")
    add_packages("cxxopts")
    add_packages("boost")
    add_packages("libyuv")
    add_packages("mbedtls")

    after_buildcmd(function (target, batchcmds)
        if is_plat("windows", "mingw") then
            -- 兼容不同工具链下的 DLL 命名：有些包产物带 lib 前缀，
            -- tennis 还可能带 CPU 后缀变体。
            local seetaface_dll_patterns = {
                {"SeetaAuthorize.dll", "libSeetaAuthorize.dll"},
                {"SeetaFaceAntiSpoofingX600.dll", "libSeetaFaceAntiSpoofingX600.dll"},
                {"SeetaFaceDetector600.dll", "libSeetaFaceDetector600.dll"},
                {"SeetaFaceLandmarker600.dll", "libSeetaFaceLandmarker600.dll"},
                {"SeetaFaceRecognizer610.dll", "libSeetaFaceRecognizer610.dll"},
                {"tennis.dll", "libtennis.dll", "libtennis_*.dll"}
            }

            local seetaface_pkg = target:pkg("seetaface6open")
            local source_root = seetaface_pkg and seetaface_pkg:installdir() or nil
            local target_dir = target:targetdir()

            if source_root then
                for _, candidates in ipairs(seetaface_dll_patterns) do
                    local copied = false
                    for _, pattern in ipairs(candidates) do
                        local matches = os.files(path.join(source_root, "**", pattern))
                        if #matches > 0 then
                            for _, match in ipairs(matches) do
                                batchcmds:cp(match, target_dir)
                            end
                            copied = true
                            break
                        end
                    end
                    if not copied then
                        print("警告: 找不到DLL文件: " .. candidates[1])
                    end
                end
            else
                print("警告: seetaface6open 包未安装，跳过 DLL 复制")
            end
        end
        batchcmds:cp("$(projectdir)/FaceRecognizer/resources", target:targetdir())
    end)



target("SampleV2CredentialProvider")
    set_kind("shared")
    set_filename("SampleV2CredentialProvider.dll")
    set_prefixname("")
    -- add_sysincludedirs("D:/Tools/llvm-mingw-ucrt-x86_64/include/c++/v1")
    add_shflags("-static-libgcc", "-static-libstdc++", {force = true})
    add_files("CredentialProvider/*.cpp")
    add_headerfiles("CredentialProvider/**.h")
    add_includedirs("common", {public = false})
    add_defines("UNICODE", "_UNICODE", "SAMPLEV2CREDENTIALPROVIDER_EXPORTS")
    apply_common_windows_settings("0x0602")
    add_syslinks("user32", "ole32", "shlwapi", "credui", "secur32", "uuid", "advapi32", "crypt32")
    add_links("Credui", "Shlwapi", "Secur32")
    add_files("CredentialProvider/samplev2credentialprovider.def")
    
    -- 资源文件处理
    add_files("CredentialProvider/resources.rc")

    add_packages("boost")

    after_buildcmd(function (target, batchcmds)
        local implibfile = target:artifactfile("implib")
        if implibfile then
            batchcmds:rm(implibfile)
        end
    end)

target("Smile2Unlock")
    apply_common_module_binary_target()
    add_files(
        "Smile2Unlock/*.cpp",
        "Smile2Unlock/modules/*.cppm",
        "common/modules/*.cppm",
        "Smile2Unlock/resources.rc"
    )
    add_includedirs("common", {public = true})
    add_includedirs("Smile2Unlock", {public = true})
    apply_common_windows_settings("0x0602")
    add_packages("glfw", "imgui", "boost", "sqlite3", "mbedtls")
    add_syslinks("opengl32", "user32", "gdi32", "shell32", "advapi32", "crypt32", "version", "secur32", "winhttp")
    add_defines(
        "SMILE2UNLOCK_VERSION=\"" .. app_version_tag .. "\"",
        "SMILE2UNLOCK_RELEASES_API=\"" .. releases_api_url .. "\"",
        "SMILE2UNLOCK_RELEASES_PAGE=\"" .. releases_page_url .. "\""
    )

    on_run(function()
        local app_version = "0.0.0"
        if os.isfile("version.txt") then
            local version_text = io.readfile("version.txt")
            if version_text then
                app_version = version_text:match("%S+") or app_version
            end
        end
        local app_version_tag = app_version
        if app_version_tag:sub(1, 1) ~= "v" and app_version_tag:sub(1, 1) ~= "V" then
            app_version_tag = "v" .. app_version_tag
        end

    end)

    after_buildcmd(function (target, batchcmds)
        copy_llvm_runtime_dlls(batchcmds, target, {
            "libunwind.dll",
            "libc++.dll",
            "libomp.dll"
        })
    end)
    after_build(function (target)
        local output_resources = path.join(target:targetdir(), "resources")
        os.mkdir(output_resources)
        os.rm(path.join(output_resources, "img"))
        os.rm(path.join(output_resources, "resources"))
        os.cp(path.join(os.projectdir(), "common", "resources", "*"), output_resources)
    end)

target("Smile2UnlockRuntimeFiles")
    set_kind("phony")
    set_default(true)
    add_deps("Smile2Unlock")
    on_build(function (target)
        local app_target = target:dep("Smile2Unlock")
        if app_target then
            local dll_names = {
                "libunwind.dll",
                "libc++.dll",
                "libomp.dll"
            }
            local resolved = find_llvm_runtime_dll_paths(dll_names)
            for _, dll_name in ipairs(dll_names) do
                local dll_path = resolved[dll_name]
                if dll_path then
                    os.cp(dll_path, app_target:targetdir())
                else
                    print("警告: 找不到 LLVM 运行时 DLL: " .. dll_name)
                end
            end
        end
    end)