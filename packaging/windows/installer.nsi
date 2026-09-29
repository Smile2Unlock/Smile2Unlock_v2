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
!define MUI_LICENSEPAGE_BUTTON "$(^InstallBtn)"
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_FUNCTION LaunchApp
!define MUI_FINISHPAGE_RUN_TEXT "启动 Smile2Unlock / Start Smile2Unlock"

!insertmacro MUI_PAGE_LICENSE "${STAGE_DIR}\Smile2Unlock\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "English"

Function LaunchApp
    Exec '"$INSTDIR\bin\Smile2Unlock.exe"'
FunctionEnd

Section "Smile2Unlock" SecMain
    ; Per-machine install: shortcuts and the uninstall entry go to the
    ; all-users locations, not the installing account's profile.
    SetShellVarContext all
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

    CreateDirectory "$SMPROGRAMS\Smile2Unlock"
    CreateShortcut "$SMPROGRAMS\Smile2Unlock\Smile2Unlock.lnk" \
        "$INSTDIR\bin\Smile2Unlock.exe" "" "$INSTDIR\bin\Smile2Unlock.ico"
    CreateShortcut "$SMPROGRAMS\Smile2Unlock\Uninstall Smile2Unlock.lnk" \
        "$INSTDIR\Uninstall.exe"
    CreateShortcut "$DESKTOP\Smile2Unlock.lnk" \
        "$INSTDIR\bin\Smile2Unlock.exe" "" "$INSTDIR\bin\Smile2Unlock.ico"

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

Section "Uninstall"
    SetShellVarContext all
    ; Order matters: the helper must still exist when we unregister the
    ; credential provider, and the service must be stopped before its
    ; files are deleted.
    nsExec::ExecToLog 'taskkill /IM Smile2Unlock.exe /F'
    Pop $0

    nsExec::ExecToLog '"$INSTDIR\package\bin\Smile2UnlockDeployHelper.exe" --unregister-cp'
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

    RMDir /r "$INSTDIR\package"
    RMDir /r "$INSTDIR\bin"
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
