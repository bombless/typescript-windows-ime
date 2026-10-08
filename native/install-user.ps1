$ErrorActionPreference = "Stop"

$buildDll = Join-Path $PSScriptRoot "build\TypeScriptWindowsIme.dll"
$installDir = Join-Path $PSScriptRoot "install"
$version = Get-Date -Format "yyyyMMdd-HHmmssfff"
$dll = Join-Path $installDir "TypeScriptWindowsIme-$version.dll"
if (-not (Test-Path $buildDll)) { throw "Native build DLL not found: $buildDll. Run .\build.ps1 first." }
$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "Administrator permission is required for TSF registration. Requesting elevation..."
    $arguments = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $PSCommandPath)
    $elevatedShell = if (Test-Path (Join-Path $PSHOME "pwsh.exe")) { Join-Path $PSHOME "pwsh.exe" } else { "powershell.exe" }
    $child = Start-Process -FilePath $elevatedShell -ArgumentList $arguments -Verb RunAs -Wait -PassThru
    if ($child.ExitCode -ne 0) {
        throw "Elevated TSF registration failed with exit code $($child.ExitCode). Open an Administrator PowerShell and run: & '$elevatedShell' -NoProfile -ExecutionPolicy Bypass -File '$PSCommandPath'"
    }
    exit 0
}

New-Item -ItemType Directory -Force -Path $installDir | Out-Null

# Never overwrite an installed DLL: TSF may still have an older version loaded.
# A unique file lets the old module finish its current lifetime while the COM
# registration switches atomically to the new build.
Copy-Item -Force $buildDll $dll -ErrorAction Stop

# DllRegisterServer performs the supported TSF registration calls: Register,
# AddLanguageProfile, and RegisterCategory. Writing HKCU values alone does not
# create a visible keyboard TIP.
$regsvr32 = Join-Path $env:WINDIR "System32\regsvr32.exe"
$process = Start-Process -FilePath $regsvr32 -ArgumentList @('/s', $dll) -Wait -PassThru
if ($process.ExitCode -ne 0) { throw "64-bit regsvr32 failed with exit code $($process.ExitCode) for $dll." }

$clsid = "{7B2E4F5A-3A8E-4D74-9F0B-6D5D6F0E6C41}"
$profile = "{C0A3B5B1-3C52-4E8E-A8A7-1F2B3D4C5E60}"
$reg = Join-Path $env:WINDIR "System32\reg.exe"
# Ensure the 64-bit COM registration is present in the same view used by the
# 64-bit TSF host. DllRegisterServer normally creates these values, but the
# explicit write makes this script robust when registry redirection is active.
$clsidKey = "HKLM\SOFTWARE\Classes\CLSID\$clsid"
$inprocKey = "$clsidKey\InprocServer32"
& $reg add $clsidKey /ve /t REG_SZ /d "TypeScript Windows IME" /f | Out-Null
& $reg add $inprocKey /ve /t REG_SZ /d $dll /f | Out-Null
& $reg add $inprocKey /v ThreadingModel /t REG_SZ /d Apartment /f | Out-Null
$comQuery = & $reg query $inprocKey /ve 2>$null
if ($LASTEXITCODE -ne 0 -or -not (($comQuery -join "`n") -like "*$dll*")) {
    throw "64-bit COM registration verification failed. Expected: $dll"
}

$profileKey = "HKLM\SOFTWARE\Microsoft\CTF\TIP\$clsid\LanguageProfile\0x00000804\$profile"
& $reg query $profileKey 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) { throw "TSF registration verification failed. Language profile was not created: $profileKey" }
& $reg add $profileKey /v Enable /t REG_DWORD /d 1 /f | Out-Null

# Windows uses the per-user Enable flag when deciding whether a TIP can be
# activated for the current language. Add it for every profile we register;
# without this value the IME can appear in the list but fail to switch on.
foreach ($langid in @("00000409", "00000804", "00000411")) {
    $userProfileKey = "HKCU\Software\Microsoft\CTF\TIP\$clsid\LanguageProfile\0x$langid\$profile"
    & $reg add $userProfileKey /v Enable /t REG_DWORD /d 1 /f | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Per-user TSF profile enable failed: $userProfileKey" }
}

Write-Host "64-bit TSF registration verified:"
Write-Host "  CLSID:   $clsid"
Write-Host "  DLL:     $dll"
Write-Host "  Profile: zh-CN ($profile)"
Write-Host "Restarting ctfmon..."
$ctfmon = Join-Path $env:WINDIR "System32\ctfmon.exe"
if (-not (Test-Path $ctfmon)) { throw "ctfmon.exe was not found at '$ctfmon'." }
Get-Process ctfmon -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Process -FilePath $ctfmon
Write-Host "Restart the target application and select 'TypeScript Windows IME'."
