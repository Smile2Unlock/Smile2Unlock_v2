add_rules("mode.debug", "mode.release")
set_languages("c++26")
target("Smile2Unlock_v2")
    set_kind("binary")
    add_files("FaceRecognizer/src/*.cpp")
    add_headerfiles("FaceRecognizer/src/*.h")
    add_includedirs("D:\\Libs\\opencv4.2\\build\\include")
    add_links("opencv_world420","opencv_world420d")
    add_links("SeetaAgePredictor600","SeetaAgePredictor600d","SeetaEyeStateDetector200","SeetaEyeStateDetector200d","SeetaFaceAntiSpoofingX600","SeetaFaceAntiSpoofingX600d","SeetaFaceDetector600","SeetaFaceDetector600d","SeetaFaceLandmarker600","SeetaFaceLandmarker600d","SeetaFaceRecognizer610","SeetaFaceRecognizer610d","SeetaFaceTracking600","SeetaFaceTracking600d","SeetaGenderPredictor600","SeetaGenderPredictor600d","SeetaMaskDetector200","SeetaMaskDetector200d","SeetaPoseEstimation600","SeetaPoseEstimation600d","SeetaQualityAssessor300","SeetaQualityAssessor300d")
    add_linkdirs("D:\\Libs\\opencv4.2\\build\\x64\\vc15\\lib")
    add_includedirs("D:\\Libs\\sf6.0_windows\\include")
    add_linkdirs("D:\\Libs\\sf6.0_windows\\lib\\x64")

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

