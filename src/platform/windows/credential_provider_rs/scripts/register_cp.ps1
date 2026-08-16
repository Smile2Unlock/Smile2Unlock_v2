# Registers the Smile2Unlock credential provider with the Windows logon
# UI. Creates/repairs both registration points:
#   1. HKLM\SOFTWARE\Classes\CLSID\{5fd3d285-0dd9-4362-8855-e0abaacd4af6}
#      (InprocServer32 -> su_credential_provider_fix_v28.dll)
#   2. HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Authentication\
#      CredentialProviders  (next free index -> the CLSID)
#
# Run elevated:  powershell -ExecutionPolicy Bypass -File register_cp.ps1
# Optional: -DllPath <path> to point the CLSID at a different DLL.

param(
    [string]$DllPath = "C:\su-deploy\bin\su_credential_provider_fix_v28.dll"
)

$ErrorActionPreference = "Stop"
$clsid = "{5fd3d285-0dd9-4362-8855-e0abaacd4af6}"
$clsidKey = "HKLM:\SOFTWARE\Classes\CLSID\$clsid"
$cpKey = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Authentication\CredentialProviders"

if (-not (Test-Path $DllPath)) {
    Write-Error "DLL not found: $DllPath"
    exit 1
}

# 1. CLSID registration (default value + InprocServer32).
New-Item -Path $clsidKey -Force | Out-Null
New-Item -Path "$clsidKey\InprocServer32" -Force | Out-Null
Set-ItemProperty -Path $clsidKey -Name "(default)" -Value "Smile2Unlock" -Type String
Set-ItemProperty -Path "$clsidKey\InprocServer32" -Name "(default)" -Value $DllPath -Type String
Set-ItemProperty -Path "$clsidKey\InprocServer32" -Name "ThreadingModel" -Value "Apartment" -Type String
Write-Host "CLSID registered -> $DllPath"

# 2. Logon-UI enrollment: add the CLSID under the next free index (1, 2, ...).
New-Item -Path $cpKey -Force | Out-Null
$index = 1
$existing = @()
while (Get-ItemProperty -Path $cpKey -Name "$index" -ErrorAction SilentlyContinue) {
    $existing += Get-ItemProperty -Path $cpKey -Name "$index" | Select-Object -ExpandProperty "$index"
    $index++
}
if ($existing -contains $clsid) {
    Write-Host "CredentialProviders already lists $clsid"
} else {
    Set-ItemProperty -Path $cpKey -Name "$index" -Value $clsid -Type String
    Write-Host "CredentialProviders[$index] = $clsid"
}

# 3. Sanity: re-read both registration points.
Write-Host "--- verification ---"
Get-ItemProperty -Path "$clsidKey\InprocServer32" | Select-Object "(default)", ThreadingModel | Format-List
Get-ItemProperty -Path $cpKey | Format-List
