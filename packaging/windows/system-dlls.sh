#!/usr/bin/env bash

# Windows components expected to be provided by supported Windows releases.
windows_is_system_dll() {
    local name="${1,,}"
    case "$name" in
        api-ms-*|ext-ms-*) return 0 ;;
        advapi32.dll|bcrypt.dll|bcryptprimitives.dll|cabinet.dll|cfgmgr32.dll) return 0 ;;
        clbcatq.dll|combase.dll|comctl32.dll|comdlg32.dll|credui.dll|crypt32.dll) return 0 ;;
        d3d9.dll|d3d11.dll|dhcpcsvc.dll|dhcpcsvc6.dll|dwrite.dll|dwmapi.dll|dxgi.dll) return 0 ;;
        dxva2.dll|evr.dll|gdi32.dll|gdiplus.dll|imm32.dll|iphlpapi.dll|kernel32.dll) return 0 ;;
        kernelbase.dll|mf.dll|mfperfhelper.dll|mfplat.dll|mfreadwrite.dll|mfuuid.dll) return 0 ;;
        msvcrt.dll|ncrypt.dll|netapi32.dll|normaliz.dll|ntdll.dll|ole32.dll|oleaut32.dll) return 0 ;;
        onecoreuap.dll|opengl32.dll|opengl32sw.dll|powrprof.dll|propsys.dll|psapi.dll) return 0 ;;
        rpcrt4.dll|secur32.dll|setupapi.dll|shell32.dll|shlwapi.dll|sspicli.dll) return 0 ;;
        ucrtbase.dll|user32.dll|userenv.dll|uxtheme.dll|version.dll|winhttp.dll) return 0 ;;
        winmm.dll|wintrust.dll|windows.storage.dll|wldp.dll|ws2_32.dll|wtsapi32.dll) return 0 ;;
        *) return 1 ;;
    esac
}
