add_rules("mode.debug", "mode.release")
add_rules("plugin.compile_commands.autoupdate", {outputdir = ".vscode"})

add_repositories("myrepo local-repo")
add_requires("seetaface6open")
add_requires("tiny-aes-c")
add_requires("cxxopts")
add_requires("boost", {configs = {asio = true}})
add_requires("libyuv")
add_requires("glfw", "imgui", {configs = {glfw = true, opengl3 = true}})
add_requires("sqlite3")

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
    add_packages("tiny-aes-c")
    add_packages("cxxopts")
    add_packages("boost")
    add_packages("libyuv")

    after_build(function (target)
        if is_plat("windows", "mingw") then
            -- 确保关键DLL被复制（显式列出以确保不遗漏）
            local seetaface_dlls = {
                "SeetaAuthorize.dll",
                "SeetaFaceAntiSpoofingX600.dll",
                "SeetaFaceDetector600.dll",
                "SeetaFaceLandmarker600.dll",
                "SeetaFaceRecognizer610.dll",
                "tennis.dll"
            }

            local seetaface_pkg = target:pkg("seetaface6open")
            local source_root = seetaface_pkg and seetaface_pkg:installdir() or nil
            local target_dir = target:targetdir()

            if source_root then
                for _, dll in ipairs(seetaface_dlls) do
                    local matches = os.files(path.join(source_root, "**", dll))
                    if #matches > 0 then
                        os.cp(matches[1], target_dir)
                    else
                        print("警告: 找不到DLL文件: " .. dll)
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

    add_packages("tiny-aes-c")
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
    add_packages("glfw", "imgui", "boost", "sqlite3", "tiny-aes-c")
    add_syslinks("opengl32", "user32", "gdi32", "shell32", "advapi32", "crypt32")




--
-- If you want to known more usage about xmake, please see https://xmake.io
--
-- ## FAQ
--
-- You can enter the project directory firstly before building project.
--
--   $ cd projectdir
--
-- 1. How to build project?
--
--   $ xmake
--
-- 2. How to configure project?
--
--   $ xmake f -p [macosx|linux|iphoneos ..] -a [x86_64|i386|arm64 ..] -m [debug|release]
--
-- 3. Where is the build output directory?
--
--   The default output directory is `./build` and you can configure the output directory.
--
--   $ xmake f -o outputdir
--   $ xmake
--
-- 4. How to run and debug target after building project?
--
--   $ xmake run [targetname]
--   $ xmake run -d [targetname]
--
-- 5. How to install target to the system directory or other output directory?
--
--   $ xmake install
--   $ xmake install -o installdir
--
-- 6. Add some frequently-used compilation flags in xmake.lua
--
-- @code
--    -- add debug and release modes
--    add_rules("mode.debug", "mode.release")
--
--    -- add macro definition
--    add_defines("NDEBUG", "_GNU_SOURCE=1")
--
--    -- set warning all as error
--    set_warnings("all", "error")
--
--    -- set language: c99, c++11
--    set_languages("c99", "c++11")
--
--    -- set optimization: none, faster, fastest, smallest
--    set_optimize("fastest")
--
--    -- add include search directories
--    add_includedirs("/usr/include", "/usr/local/include")
--
--    -- add link libraries and search directories
--    add_links("tbox")
--    add_linkdirs("/usr/local/lib", "/usr/lib")
--
--    -- add system link libraries
--    add_syslinks("z", "pthread")
--
--    -- add compilation and link flags
--    add_cxflags("-stdnolib", "-fno-strict-aliasing")
--    add_ldflags("-L/usr/local/lib", "-lpthread", {force = true})
--
-- @endcode
--
