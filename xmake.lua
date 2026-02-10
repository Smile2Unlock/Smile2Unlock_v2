add_rules("mode.debug", "mode.release")
add_rules("plugin.compile_commands.autoupdate", {outputdir = ".vscode"})

add_repositories("myrepo local-repo")
add_requires("SeetaFace6Open")
add_requires("cryptopp")
add_requires("cxxopts")
add_requires("boost", {configs = {asio = true}})

target("FaceRecognizer")
    set_languages("c++26")
    set_policy("build.c++.modules", true)
    set_kind("binary")
    add_files("FaceRecognizer/src/*.cpp")
    add_headerfiles("FaceRecognizer/src/**.h")
    add_headerfiles("FaceRecognizer/src/**.hpp")
    add_includedirs("FaceRecognizer/src", {public = false})
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
    set_languages("c++26")
    set_kind("shared")
    set_arch("x64")
    add_files("CredentialProvider/*.cpp")
    add_headerfiles("CredentialProvider/**.h")
    add_includedirs("CredentialProvider", {public = false})
    add_defines("UNICODE", "_UNICODE", "SAMPLEV2CREDENTIALPROVIDER_EXPORTS", "_WIN32_WINNT=0x0602")  -- Windows 8
    add_syslinks("user32", "ole32", "shlwapi", "credui", "secur32", "uuid", "advapi32")
    add_links("Credui", "Shlwapi", "Secur32")
    add_files("CredentialProvider/samplev2credentialprovider.def")

    -- 资源文件处理
    add_files("CredentialProvider/resources.rc")

    add_packages("cryptopp")
    add_packages("boost")




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