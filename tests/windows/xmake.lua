add_rules("mode.debug", "mode.release")

add_requires("boost 1.90.0", {
    configs = {asio = true, cmake = false, header_only = true}
})

set_languages("c++23")

target("windows_storage_provider_check")
    set_kind("static")
    add_files("../../src/platform/windows/security/storage_key_provider.cpp")
    add_files("../../src/platform/windows/security/logon_secret_store.cpp")
    add_files("../../CredentialProvider/logon_secret_client.cpp")
    add_files("../../src/platform/windows/auth_service/logon_secret_server.cpp")
    add_includedirs(
        "../../src/platform/windows/security",
        "../../src/platform/windows/auth_service",
        "../../src/core-rs/include",
        "../../CredentialProvider",
        "../../common")
    add_cxxflags("-Wall", "-Wextra", "-Wpedantic")
    add_syslinks("ncrypt", "bcrypt", "crypt32", "shell32", "ole32", "advapi32")

target("Smile2UnlockAuthService")
    set_kind("binary")
    set_runtimes("stdc++_static")
    set_targetdir("../../build/windows-storage-check")
    add_files("../../src/platform/windows/security/storage_key_provider.cpp")
    add_files("../../src/platform/windows/security/logon_secret_store.cpp")
    add_files("../../src/platform/windows/auth_service/logon_secret_server.cpp")
    add_files("../../src/platform/windows/auth_service/service_main.cpp")
    add_includedirs(
        "../../src/platform/windows/security",
        "../../src/platform/windows/auth_service",
        "../../src/core-rs/include",
        "../../common")
    add_linkdirs("../../build/cargo/x86_64-pc-windows-gnu/release")
    add_links("su_core")
    add_cxxflags("-Wall", "-Wextra", "-Wpedantic")
    add_ldflags("-static", "-municode", "-mwindows", {force = true})
    add_syslinks(
        "ncrypt", "bcrypt", "crypt32", "shell32", "ole32", "advapi32",
        "userenv", "ntdll", "ws2_32", "uuid")
    before_build(function ()
        os.execv("cargo", {
            "build",
            "--release",
            "--target", "x86_64-pc-windows-gnu",
            "--manifest-path", "../../src/core-rs/Cargo.toml",
            "--target-dir", "../../build/cargo"
        })
    end)

target("SampleV2CredentialProvider")
    set_kind("shared")
    set_runtimes("stdc++_static")
    set_prefixname("")
    set_filename("SampleV2CredentialProvider.dll")
    set_targetdir("../../build/windows-storage-check")
    add_files(
        "../../CredentialProvider/CSampleCredential.cpp",
        "../../CredentialProvider/CSampleProvider.cpp",
        "../../CredentialProvider/Dll.cpp",
        "../../CredentialProvider/guid.cpp",
        "../../CredentialProvider/helpers.cpp",
        "../../CredentialProvider/logon_secret_client.cpp",
        "../../CredentialProvider/samplev2credentialprovider.def")
    add_includedirs("../../CredentialProvider", "../../common")
    add_packages("boost")
    add_defines("UNICODE", "_UNICODE", "NOMINMAX", "WIN32_LEAN_AND_MEAN")
    add_cxxflags("-Wall", "-Wextra", "-Wpedantic")
    add_shflags("-static", {force = true})
    add_syslinks(
        "advapi32", "credui", "ole32", "secur32", "shell32", "shlwapi",
        "user32", "uuid", "ws2_32")
