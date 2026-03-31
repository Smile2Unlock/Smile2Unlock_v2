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

local function apply_common_windows_settings(winver)
    if is_plat("windows", "mingw") then
        add_defines("_WIN32_WINNT=" .. winver, "NOMINMAX", "_CRT_SECURE_NO_WARNINGS", "WIN32_LEAN_AND_MEAN")
        add_links("Advapi32", "mfplat", "mf", "mfreadwrite", "mfuuid", "ole32", "uuid")
    end
end



target("FaceRecognizer")
    set_encodings("utf-8")
    set_languages("c++26")
    set_plat("mingw")
    set_toolchains("clang")
    set_policy("build.c++.modules", true)
    set_kind("binary")
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

    after_build(function (target)
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
                                os.cp(match, target_dir)
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
        os.cp("$(projectdir)/FaceRecognizer/resources", target:targetdir())
    end)



target("SampleV2CredentialProvider")
    set_encodings("utf-8")
    set_languages("c++26")
    set_kind("shared")
    set_arch("x64")
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

target("Smile2Unlock")
    set_kind("binary")
    set_plat("mingw")
    set_toolchains("clang")
    set_encodings("utf-8")
    set_languages("c++26")
    set_policy("build.c++.modules", true)
    add_files(
        "Smile2Unlock/*.cpp",
        "Smile2Unlock/modules/*.cppm",
        "common/modules/*.cppm"
    )
    add_includedirs("common", {public = true})
    apply_common_windows_settings("0x0602")
    add_packages("glfw", "imgui", "boost", "sqlite3", "mbedtls")
    add_syslinks("opengl32", "user32", "gdi32", "shell32", "advapi32", "crypt32", "version")

