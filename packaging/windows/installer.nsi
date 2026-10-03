; Smile2Unlock NSIS installer (thin wrapper).
;
; The installer deliberately implements NO privileged logic of its own: it
; extracts the same signed package as the ZIP release beside the audited
; su_deploy_helper, then runs
;   su_deploy_helper --verify, then --register-cp --ensure-service
; so manifest verification (pinned internal certificate), service install
; and credential-provider registration all stay on the reviewed code path.
;
; Built by packaging/windows/package.sh right after the signed ZIP, from the
; same staged tree, so ZIP, installer and release-info.json can never drift.
; The setup exe itself is Authenticode-signed with the same certificate.
;
; Requires NSIS >= 3.08 (unicode). MUI2 + Stubs + Includes must be installed.

!ifndef VERSION
!error "VERSION must be passed with -DVERSION=<x.y.z>"
!endif
!ifndef STAGE_DIR
!error "STAGE_DIR must point at the staged Smile2Unlock package root's parent"
!endif

Unicode true
ManifestDPIAware true
SetCompressor /SOLID lzma
Name "Smile2Unlock"
!ifndef OUT_FILE
!define OUT_FILE "smile2unlock-setup.exe"
!endif
OutFile "${OUT_FILE}"
InstallDir "$PROGRAMFILES64\Smile2Unlock"
InstallDirRegKey HKLM "Software\Smile2Unlock" "InstallLocation"
RequestExecutionLevel admin

!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Smile2Unlock"

!include MUI2.nsh
!include LogicLib.nsh

!define MUI_ABORTWARNING
!define MUI_ICON "${NSISDIR}\Contrib\Graphics\Icons\orange-install.ico"
!define MUI_UNICON "${NSISDIR}\Contrib\Graphics\Icons\orange-uninstall.ico"
; The product LICENSE (MIT) is shown as-is; THIRD-PARTY-NOTICES.md and the
; full license texts are installed next to the binaries and shown in the
; details log.
!insertmacro MUI_PAGE_LICENSE "${STAGE_DIR}\Smile2Unlock\LICENSE"
!define MUI_PAGE_HEADER_TEXT "$(ShortcutsTitle)"
!define MUI_PAGE_HEADER_SUBTEXT "$(ShortcutsSubtitle)"
!define MUI_COMPONENTSPAGE_TEXT_TOP "$(ShortcutsDescription)"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_FUNCTION LaunchApp
!define MUI_FINISHPAGE_RUN_TEXT "$(LaunchAppText)"
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "English"

LangString ShortcutsTitle ${LANG_SIMPCHINESE} "快捷方式"
LangString ShortcutsTitle ${LANG_ENGLISH} "Shortcuts"
LangString ShortcutsSubtitle ${LANG_SIMPCHINESE} "选择要创建的快捷方式。"
LangString ShortcutsSubtitle ${LANG_ENGLISH} "Choose which shortcuts to create."
LangString ShortcutsDescription ${LANG_SIMPCHINESE} "桌面和开始菜单快捷方式均为可选。Smile2Unlock 主程序必须安装。"
LangString ShortcutsDescription ${LANG_ENGLISH} "Desktop and Start Menu shortcuts are optional. Smile2Unlock is required."
LangString DesktopShortcutText ${LANG_SIMPCHINESE} "桌面快捷方式"
LangString DesktopShortcutText ${LANG_ENGLISH} "Desktop shortcut"
LangString StartMenuShortcutText ${LANG_SIMPCHINESE} "开始菜单快捷方式"
LangString StartMenuShortcutText ${LANG_ENGLISH} "Start Menu shortcuts"
LangString MainDescription ${LANG_SIMPCHINESE} "安装 Smile2Unlock 主程序与身份认证组件。"
LangString MainDescription ${LANG_ENGLISH} "Install Smile2Unlock and its authentication components."
LangString DesktopDescription ${LANG_SIMPCHINESE} "在所有用户的桌面上创建 Smile2Unlock 快捷方式。"
LangString DesktopDescription ${LANG_ENGLISH} "Create a Smile2Unlock shortcut on the desktop for all users."
LangString StartMenuDescription ${LANG_SIMPCHINESE} "在所有用户的开始菜单中添加 Smile2Unlock 和卸载快捷方式。"
LangString StartMenuDescription ${LANG_ENGLISH} "Add Smile2Unlock and uninstall shortcuts to the Start Menu for all users."
LangString LaunchAppText ${LANG_SIMPCHINESE} "打开 Smile2Unlock"
LangString LaunchAppText ${LANG_ENGLISH} "Open Smile2Unlock"

Function LaunchApp
    SetOutPath "$INSTDIR\bin"
    Exec '"$INSTDIR\bin\Smile2Unlock.exe"'
FunctionEnd

Section "Smile2Unlock" SecMain
    SectionIn RO
    ; Per-machine install: shortcuts and the uninstall entry go to the
    ; all-users locations, not the installing account's profile.
    SetShellVarContext all
    ; Verify a separate package before the helper stops/replaces the service.
    ; Extracting over the running service first would mix old payload bytes
    ; with the new signed manifest during an upgrade.
    SetOutPath "$INSTDIR\package"
    File /r "${STAGE_DIR}\Smile2Unlock\*.*"

    DetailPrint "验证签名清单..."
    DetailPrint "Verifying the signed manifest..."
    ; su_deploy_helper refuses --verify combined with deployment operations,
    ; so run the manifest check and the deployment as two steps.
    nsExec::ExecToLog '"$INSTDIR\package\bin\Smile2UnlockDeployHelper.exe" --verify'
    Pop $0
    ${If} $0 != 0
        DetailPrint "manifest verification failed (exit $0)"
        ; The helper writes the precise reason (Authenticode status, hash
        ; mismatch, missing manifest entry, ...) to %TEMP%\su_deploy_result.json;
        ; surface it in the dialog so failures are diagnosable off-site.
        StrCpy $2 ""
        ClearErrors
        FileOpen $3 "$TEMP\su_deploy_result.json" r
        ${Unless} ${Errors}
            FileRead $3 $2
            FileClose $3
        ${EndUnless}
        MessageBox MB_ICONSTOP \
            "签名清单校验失败（退出码 $0）。$\n$\n$2$\n$\nManifest verification failed (exit code $0)." \
            /SD IDOK
        SetErrorLevel $0
        Abort
    ${EndIf}

    DetailPrint "部署服务与凭据提供者..."
    DetailPrint "Installing the service and credential provider..."
    ; A service removed moments ago lingers marked-for-delete for a few
    ; seconds; the helper reports that as a retryable failure, so try again
    ; before giving up.
    StrCpy $1 0
    ${While} $1 < 3
        nsExec::ExecToLog '"$INSTDIR\package\bin\Smile2UnlockDeployHelper.exe" --register-cp --ensure-service'
        Pop $0
        ${If} $0 == 0
            ${Break}
        ${EndIf}
        IntOp $1 $1 + 1
        ${If} $1 < 3
            DetailPrint "retrying deployment (attempt $1, exit $0)..."
            Sleep 2000
        ${EndIf}
    ${EndWhile}
    ${If} $0 != 0
        DetailPrint "su_deploy_helper failed (exit $0)"
        StrCpy $2 ""
        ClearErrors
        FileOpen $3 "$TEMP\su_deploy_result.json" r
        ${Unless} ${Errors}
            FileRead $3 $2
            FileClose $3
        ${EndUnless}
        MessageBox MB_ICONSTOP \
            "部署失败（退出码 $0）。$\n$\n$2$\n$\nDeployment failed (exit code $0)." \
            /SD IDOK
        SetErrorLevel $0
        Abort
    ${EndIf}

    ; The helper has installed bin/ and assets/ and restarted the service.
    ; Publish only the remaining package metadata and license files: writing
    ; the payload again would try to overwrite the running service and DLLs.
    SetOutPath "$INSTDIR"
    File /r /x bin /x assets "${STAGE_DIR}\Smile2Unlock\*.*"

    ; Remove only our known shortcuts before optional sections recreate the
    ; selected ones. This also applies deselections during an upgrade.
    Delete "$DESKTOP\Smile2Unlock.lnk"
    Delete "$SMPROGRAMS\Smile2Unlock\Smile2Unlock.lnk"
    Delete "$SMPROGRAMS\Smile2Unlock\Uninstall Smile2Unlock.lnk"
    RMDir "$SMPROGRAMS\Smile2Unlock"

    SetRegView 64
    WriteRegStr HKLM "Software\Smile2Unlock" "InstallLocation" "$INSTDIR"
    WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "Smile2Unlock"
    WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
    WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "Smile2Unlock contributors"
    WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\bin\Smile2Unlock.exe"
    WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
    ; Windows' "Apps & features" entry point; the path contains spaces, so
    ; quote it.
    WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
    WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
    WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
    WriteUninstaller "$INSTDIR\Uninstall.exe"
SectionEnd

; NSIS preserves optional section choices when navigating Back/Next.
; Desktop is opt-in; the Start Menu remains selected by default.
Section /o "$(DesktopShortcutText)" SecDesktop
    SetShellVarContext all
    SetOutPath "$INSTDIR\bin"
    CreateShortcut "$DESKTOP\Smile2Unlock.lnk" \
        "$INSTDIR\bin\Smile2Unlock.exe" "" "$INSTDIR\bin\Smile2Unlock.ico"
SectionEnd

Section "$(StartMenuShortcutText)" SecStartMenu
    SetShellVarContext all
    SetOutPath "$INSTDIR\bin"
    CreateDirectory "$SMPROGRAMS\Smile2Unlock"
    CreateShortcut "$SMPROGRAMS\Smile2Unlock\Smile2Unlock.lnk" \
        "$INSTDIR\bin\Smile2Unlock.exe" "" "$INSTDIR\bin\Smile2Unlock.ico"
    CreateShortcut "$SMPROGRAMS\Smile2Unlock\Uninstall Smile2Unlock.lnk" \
        "$INSTDIR\Uninstall.exe"
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
    !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} "$(MainDescription)"
    !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "$(DesktopDescription)"
    !insertmacro MUI_DESCRIPTION_TEXT ${SecStartMenu} "$(StartMenuDescription)"
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
    SetShellVarContext all
    SetOutPath "$TEMP"
    ; Order matters: the helper must still exist when we unregister the
    ; credential provider, and the service must be stopped before its
    ; files are deleted.
    nsExec::ExecToLog 'taskkill /IM Smile2Unlock.exe /F'
    Pop $0

    nsExec::ExecToLog '"$INSTDIR\bin\Smile2UnlockDeployHelper.exe" --unregister-cp'
    Pop $0

    nsExec::ExecToLog 'sc stop Smile2UnlockAuthService'
    Pop $0
    Sleep 1500
    nsExec::ExecToLog 'sc delete Smile2UnlockAuthService'
    Pop $0

    Delete "$SMPROGRAMS\Smile2Unlock\Smile2Unlock.lnk"
    Delete "$SMPROGRAMS\Smile2Unlock\Uninstall Smile2Unlock.lnk"
    RMDir "$SMPROGRAMS\Smile2Unlock"
    Delete "$DESKTOP\Smile2Unlock.lnk"

    RMDir /r "$INSTDIR\bin"
    ; Remove the nested tree left by installers before the package-layout fix.
    RMDir /r "$INSTDIR\package"
    RMDir /r "$INSTDIR\assets"
    RMDir /r "$INSTDIR\licenses"
    Delete "$INSTDIR\LICENSE"
    Delete "$INSTDIR\THIRD-PARTY-NOTICES.md"
    Delete "$INSTDIR\Uninstall.exe"
    RMDir "$INSTDIR"

    SetRegView 64
    DeleteRegKey HKLM "${UNINST_KEY}"
    DeleteRegValue HKLM "Software\Smile2Unlock" "InstallLocation"
    ; Per-user encrypted profiles and the storage master key under
    ; C:\ProgramData\Smile2Unlock are intentionally preserved.
SectionEnd
