-- Project-local ONNX Runtime package (CPU only, official Microsoft NuGet
-- binaries). The system/xmake-repo packages are avoided on purpose: a distro
-- build may be the CUDA flavour, and GitHub Release downloads are unreliable.
package("onnxruntime")
    set_homepage("https://www.onnxruntime.ai")
    set_description("ONNX Runtime: cross-platform, high performance ML inferencing (CPU)")
    set_license("MIT")

    add_configs("shared", {description = "Download shared binaries.", default = true, type = "boolean", readonly = true})

    set_urls("https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/$(version)/microsoft.ml.onnxruntime.$(version).nupkg")
    add_versions("1.22.0", "d571e63a2329baacb713f441e65ad75284de354db6e1ac435fe4bebbb417986a")

    on_install("linux|x86_64", "linux|arm64", "windows|x64", "mingw|x86_64", function (package)
        import("utils.archive")
        local origin = assert(package:originfile(), "onnxruntime nupkg origin file not found")
        local workdir = package:builddir()
        local zipfile = path.join(workdir, "ort.zip")
        local extracted = path.join(workdir, "extracted")
        os.rm(zipfile)
        os.rm(extracted)
        os.cp(origin, zipfile)
        archive.extract(zipfile, extracted)

        os.mkdir(package:installdir("include"))
        os.cp(path.join(extracted, "build", "native", "include", "*"), package:installdir("include"))
        for _, name in ipairs({"LICENSE", "ThirdPartyNotices.txt"}) do
            local src = path.join(extracted, name)
            if os.isfile(src) then
                os.cp(src, package:installdir())
            end
        end

        if package:is_plat("linux") then
            local rid = (package:is_arch("arm64", "aarch64")) and "linux-arm64" or "linux-x64"
            local nativedir = path.join(extracted, "runtimes", rid, "native")
            local libdir = package:installdir("lib")
            os.mkdir(libdir)
            local ver = package:version_str()
            local soname_target = "libonnxruntime.so." .. ver
            os.cp(path.join(nativedir, "libonnxruntime.so"), path.join(libdir, soname_target))
            os.ln(soname_target, path.join(libdir, "libonnxruntime.so.1"))
            os.ln("libonnxruntime.so.1", path.join(libdir, "libonnxruntime.so"))
            local shared_prov = path.join(nativedir, "libonnxruntime_providers_shared.so")
            if os.isfile(shared_prov) then
                os.cp(shared_prov, libdir)
            end
        else
            local nativedir = path.join(extracted, "runtimes", "win-x64", "native")
            os.mkdir(package:installdir("lib"))
            os.mkdir(package:installdir("bin"))
            for _, file in ipairs(os.files(path.join(nativedir, "*.lib"))) do
                os.cp(file, package:installdir("lib"))
            end
            for _, file in ipairs(os.files(path.join(nativedir, "*.dll"))) do
                os.cp(file, package:installdir("bin"))
            end
        end
    end)

    on_fetch(function (package)
        local includedir = package:installdir("include")
        local libdir = package:installdir("lib")
        local bindir = package:installdir("bin")
        if not os.isfile(path.join(includedir, "onnxruntime_cxx_api.h")) then
            return nil
        end
        local has_library = package:is_plat("linux")
            and os.isfile(path.join(libdir, "libonnxruntime.so"))
            or os.isfile(path.join(libdir, "onnxruntime.lib"))
        if not has_library then
            return nil
        end
        return {
            links = {"onnxruntime"},
            linkdirs = {libdir},
            sysincludedirs = {includedir},
            bindirs = {bindir}
        }
    end)

    on_test(function (package)
        assert(os.isfile(path.join(package:installdir("include"), "onnxruntime_cxx_api.h")),
            "onnxruntime headers not found")
    end)
