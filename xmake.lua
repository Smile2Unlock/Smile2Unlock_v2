add_rules("mode.debug", "mode.release")
add_rules("plugin.compile_commands.autoupdate", {outputdir = ".vscode"})

-- 全局定义,避免 winsock 冲突
add_defines("WIN32_LEAN_AND_MEAN", "_WINSOCKAPI_")

add_repositories("myrepo local-repo")
add_requires("SeetaFace6Open")
add_requires("cryptopp")
add_requires("cxxopts")
add_requires("boost", {configs = {asio = true}})
add_requires("glfw", "imgui", {configs = {glfw = true, opengl3 = true}})

target("FaceRecognizer")
    set_encodings("utf-8")
    set_languages("c++26")
    -- set_policy("build.c++.modules", true)  -- FaceRecognizer 使用传统 cpp 文件,不使用模块
    set_kind("binary")
    add_files("FaceRecognizer/src/*.cpp")
    add_headerfiles("FaceRecognizer/src/**.h")
    add_headerfiles("FaceRecognizer/src/**.hpp")
    add_includedirs("common", {public = false})    add_includedirs("FaceRecognizer/src", {public = false})
    add_defines("_WIN32_WINNT=0x0602")  -- Windows 8 
    add_packages("SeetaFace6Open")
    add_packages("cryptopp")
    add_packages("cxxopts")
    add_packages("boost")
    add_links("Advapi32")
    add_links("mfplat", "mf", "mfreadwrite", "mfuuid", "ole32", "uuid")
    add_cxflags("/DWIN32_LEAN_AND_MEAN")

    after_build(function (target)
        if is_plat("windows") then
            -- 确保关键DLL被复制（显式列出以确保不遗漏）
            local seetaface_dlls = {
                "SeetaAuthorize.dll",
                "SeetaEyeStateDetector200.dll",
                "SeetaFaceAntiSpoofingX600.dll",
                "SeetaFaceDetector600.dll",
                "SeetaFaceLandmarker600.dll",
                "SeetaFaceRecognizer610.dll",
                "tennis.dll"
            }

            local source_dir = "$(projectdir)/local-repo/packages/s/SeetaFace6Open/windows/lib"
            local target_dir = target:targetdir()

            for _, dll in ipairs(seetaface_dlls) do
                local source_file = path.join(source_dir, dll)
                if os.isfile(source_file) then
                    os.cp(source_file, target_dir)
                else
                    print("警告: 找不到DLL文件: " .. dll)
                end
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
    add_defines("UNICODE", "_UNICODE", "SAMPLEV2CREDENTIALPROVIDER_EXPORTS", "_WIN32_WINNT=0x0602")  -- Windows 8
    add_syslinks("user32", "ole32", "shlwapi", "credui", "secur32", "uuid", "advapi32")
    add_links("Credui", "Shlwapi", "Secur32")
    add_files("CredentialProvider/samplev2credentialprovider.def")
    
    -- 资源文件处理
    add_files("CredentialProvider/resources.rc")

    add_packages("cryptopp")
    add_packages("boost")


-- Smile2Unlock Backend Library
target("Smile2UnlockBackend")
    set_kind("static")
    set_encodings("utf-8")
    set_languages("c++26")
    add_files(
        "Smile2Unlock/backend/service.cpp",
        "Smile2Unlock/backend/managers/*.cpp"
    )
    add_headerfiles("Smile2Unlock/backend/**.h")
    add_includedirs(".", {public = true})
    add_defines("_WIN32_WINNT=0x0602", "WIN32_LEAN_AND_MEAN")
    add_packages("cryptopp", "boost")  -- 添加 Boost (UDP 通信)
    add_syslinks("advapi32", "version", "crypt32")

-- Smile2Unlock GUI
target("Smile2Unlock")
    set_kind("binary")
    set_encodings("utf-8")
    set_languages("c++26")
    set_default(true)
    add_deps("Smile2UnlockBackend")
    add_files("Smile2Unlock/Smile2unlock.cpp", "Smile2Unlock/frontend/*.cpp")
    add_headerfiles("Smile2Unlock/frontend/**.h", "Smile2Unlock/backend/utils/**.h")
    add_includedirs(".")
    add_defines("_WIN32_WINNT=0x0602")
    add_packages("glfw", "imgui", "boost", "cryptopp")
    add_syslinks("opengl32", "gdi32", "user32", "shell32")
