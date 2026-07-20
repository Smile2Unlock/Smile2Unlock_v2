$ErrorActionPreference = "Stop"

$ProjectDir = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$ConfigDir = Join-Path $ProjectDir "build/xmake-config-windows-storage"
$OutputDir = Join-Path $ProjectDir "build/windows-storage-check"
$InstallerDir = Join-Path $ProjectDir "installer-files"

Push-Location $ProjectDir
try {
    $env:XMAKE_CONFIGDIR = $ConfigDir
    xmake build -P tests/windows
    if ($LASTEXITCODE -ne 0) {
        throw "Windows storage build failed"
    }

    New-Item -ItemType Directory -Force -Path $InstallerDir | Out-Null
    Copy-Item -Force `
        (Join-Path $OutputDir "Smile2UnlockAuthService.exe") `
        (Join-Path $InstallerDir "Smile2UnlockAuthService.exe")
    Copy-Item -Force `
        (Join-Path $OutputDir "SampleV2CredentialProvider.dll") `
        (Join-Path $InstallerDir "SampleV2CredentialProvider.dll")
} finally {
    Pop-Location
}
