$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$dll = Join-Path $PSScriptRoot "build\TypeScriptWindowsIme.dll"

if (-not (Test-Path $dll)) {
    throw "Native DLL not found: $dll. Run .\build.ps1 first."
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "install.ps1 must be run from an elevated Administrator PowerShell. Right-click PowerShell -> Run as administrator, then run: .\native\install.ps1"
}

$regsvr32 = Join-Path $env:WINDIR "System32\regsvr32.exe"
Write-Host "Registering COM server: $dll"
& $regsvr32 /s $dll
if ($LASTEXITCODE -ne 0) {
    throw "regsvr32 failed with exit code $LASTEXITCODE."
}

$clsidPath = "HKLM:\SOFTWARE\Classes\CLSID\{7B2E4F5A-3A8E-4D74-9F0B-6D5D6F0E6C41}\InprocServer32"
$serverPath = (Get-ItemProperty -Path $clsidPath -ErrorAction Stop).'(default)'
if ($serverPath -ne $dll) {
    throw "COM registration points to '$serverPath' instead of '$dll'."
}

Write-Host "COM registration verified."
Write-Host "TSF profiles are registered by DllRegisterServer for en-US, zh-CN, and ja-JP."
Write-Host ""
Write-Host "Next:"
Write-Host "  1. Keep the TypeScript server running: npm run dev"
Write-Host "  2. Restart the target application (or restart ctfmon.exe)."
Write-Host "  3. Select 'TypeScript Windows IME' from the Windows input switcher."
Write-Host "  4. Test: type ni then Space; expected result is 你."
