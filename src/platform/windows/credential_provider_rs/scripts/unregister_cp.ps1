# Removes the Smile2Unlock credential provider from the logon UI.
# Run elevated:  powershell -ExecutionPolicy Bypass -File unregister_cp.ps1
# The CLSID registration is removed too; DLLs are left in place.

$ErrorActionPreference = "Stop"
$clsid = "{5fd3d285-0dd9-4362-8855-e0abaacd4af6}"
$clsidKey = "HKLM:\SOFTWARE\Classes\CLSID\$clsid"
$cpKey = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Authentication\CredentialProviders"

# 1. Drop the CLSID from the CredentialProviders list and renumber.
if (Test-Path $cpKey) {
    $props = Get-ItemProperty -Path $cpKey
    $names = $props.PSObject.Properties.Name | Where-Object { $_ -match '^\d+$' } | Sort-Object { [int]$_ }
    $remaining = @()
    foreach ($name in $names) {
        $value = $props.$name
        if ($value -eq $clsid) {
            Write-Host "Removing CredentialProviders[$name] = $clsid"
            Remove-ItemProperty -Path $cpKey -Name $name
        } else {
            $remaining += [pscustomobject]@{ Name = [int]$name; Value = $value }
        }
    }
    # Renumber the remaining indices to be contiguous from 1.
    $i = 1
    foreach ($entry in $remaining | Sort-Object Name) {
        $old = "$($entry.Name)"
        if ($old -ne "$i") {
            Set-ItemProperty -Path $cpKey -Name "$i" -Value $entry.Value -Type String
            Remove-ItemProperty -Path $cpKey -Name $old
        }
        $i++
    }
}

# 2. Remove the CLSID registration.
if (Test-Path $clsidKey) {
    Remove-Item -Path $clsidKey -Recurse -Force
    Write-Host "Removed $clsidKey"
}

Write-Host "Credential provider unregistered."
