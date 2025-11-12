add_rules("mode.debug", "mode.release")

add_repositories("myrepo local-repo")
add_requires("SeetaFace6Open")
add_requires("localopencv")

target("FaceRecognizer")
    set_languages("c++26")
    set_policy("build.c++.modules", true)
    set_kind("binary")
    add_files("FaceRecognizer/src/*.cpp")
    add_files("FaceRecognizer/Face.cpp")
    add_headerfiles("FaceRecognizer/src/*.h")
    add_packages("SeetaFace6Open")
    add_packages("localopencv")

    after_build(function (target)
        if is_plat("windows") then
            os.cp("$(projectdir)/local-repo/packages/l/localopencv/windows/opencv/build/x64/vc16/bin/*.dll", target:targetdir())
            os.cp("$(projectdir)/local-repo/packages/s/SeetaFace6Open/windows/lib/*.dll", target:targetdir())
        end
        os.cp("$(projectdir)/FaceRecognizer/resources", target:targetdir())
    end)

target("FaceRecognizelib")
    set_languages("c++26")
    set_policy("build.c++.modules", true)
    set_kind("shared")
    add_files("FaceRecognizer/src/*.cpp")
    add_files("FaceRecognizer/DllLoader.cpp")
    add_headerfiles("FaceRecognizer/DllLoader.h")
    add_headerfiles("FaceRecognizer/src/*.h")
    add_packages("SeetaFace6Open")
    add_packages("localopencv")
    add_defines("FRLIBRARY_EXPORTS")


target("CredentialProvider")
    set_languages("c++17")
    set_kind("shared")
    add_files("CredentialProvider/*.cpp")
    add_headerfiles("CredentialProvider/*.h")
    add_defines("UNICODE", "_UNICODE") -- 添加这一行，强制使用 Unicode API
    add_syslinks("user32", "ole32", "shlwapi", "credui", "secur32", "uuid", "advapi32")
    add_deps("FaceRecognizelib")

-- TODO: 将resources内的文件复制到exe目录


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

