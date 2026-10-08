$ErrorActionPreference = "Stop"

$finalDll = Join-Path $PSScriptRoot "build\TypeScriptWindowsIme.dll"
$activeDll = Join-Path $PSScriptRoot "build\TypeScriptWindowsIme.Active.dll"
$dll = if (Test-Path $activeDll) { $activeDll } else { $finalDll }
$clsid = "{7B2E4F5A-3A8E-4D74-9F0B-6D5D6F0E6C41}"
$profile = "{C0A3B5B1-3C52-4E8E-A8A7-1F2B3D4C5E60}"

if (-not (Test-Path $dll)) {
    throw "Native DLL not found: $dll. Run .\native\build.ps1 first."
}

# COM CLSID registration is WOW64-sensitive. Always write the 64-bit view
# because TypeScriptWindowsIme.dll is built as x64.
$reg = if ([IntPtr]::Size -eq 4) {
    Join-Path $env:WINDIR "SysNative\reg.exe"
} else {
    Join-Path $env:WINDIR "System32\reg.exe"
}

$clsidPath = "HKCU\Software\Classes\CLSID\$clsid"
$inprocPath = "$clsidPath\InprocServer32"

& $reg add $clsidPath /ve /t REG_SZ /d "TypeScript Windows IME" /f | Out-Null
& $reg add $inprocPath /ve /t REG_SZ /d $dll /f | Out-Null
& $reg add $inprocPath /v ThreadingModel /t REG_SZ /d Apartment /f | Out-Null

$registered = (& $reg query $inprocPath /v "(Default)" 2>$null | Select-String "REG_SZ").ToString()
if ($registered -notlike "*$dll*") {
    throw "64-bit per-user COM registration verification failed. Expected: $dll"
}

# Keep the current Chinese language profile enabled.
$profilePath = "HKCU\Software\Microsoft\CTF\TIP\$clsid\LanguageProfile\0x00000804\$profile"
& $reg add $profilePath /v Enable /t REG_DWORD /d 1 /f | Out-Null

Write-Host "64-bit per-user COM registration verified:"
Write-Host "  CLSID: $clsid"
Write-Host "  DLL:   $dll"
Write-Host "  Profile: zh-CN enabled"
Write-Host ""
Write-Host "Restarting ctfmon..."
$ctfmon = if ([IntPtr]::Size -eq 4) {
    Join-Path $env:WINDIR "SysNative\ctfmon.exe"
} else {
    Join-Path $env:WINDIR "System32\ctfmon.exe"
}
Get-Process ctfmon -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Process $ctfmon
Write-Host ""
Write-Host "Next:"
Write-Host "  1. Restart the target application."
Write-Host "  2. Select 'TypeScript Windows IME' in the Windows input switcher."
Write-Host "  3. Type ni then Space; expected result: 你"
