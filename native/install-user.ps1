$ErrorActionPreference = "Stop"

$buildDll = Join-Path $PSScriptRoot "build\TypeScriptWindowsIme.dll"
$installDir = Join-Path $PSScriptRoot "install"
$version = Get-Date -Format "yyyyMMdd-HHmmssfff"
$dll = Join-Path $installDir "TypeScriptWindowsIme-$version.dll"
$clsid = "{7B2E4F5A-3A8E-4D74-9F0B-6D5D6F0E6C41}"
$profile = "{C0A3B5B1-3C52-4E8E-A8A7-1F2B3D4C5E60}"
if (-not (Test-Path $buildDll)) { throw "Native build DLL not found: $buildDll. Run .\build.ps1 first." }

# Windows loads the DLL the COM registration points at, so installing a build
# older than the registered one silently rolls the IME back and the fixed code
# never runs. Reject that before asking for elevation.
$registeredKey = "HKLM:\SOFTWARE\Classes\CLSID\$clsid\InprocServer32"
$registeredDll = $null
if (Test-Path -LiteralPath $registeredKey) {
    $registeredDll = (Get-ItemProperty -LiteralPath $registeredKey).'(default)'
}
$alreadyRegistered = $false
if ($registeredDll -and (Test-Path -LiteralPath $registeredDll)) {
    $build = Get-Item -LiteralPath $buildDll
    $installed = Get-Item -LiteralPath $registeredDll
    $buildHash = (Get-FileHash -LiteralPath $buildDll -Algorithm SHA256).Hash
    $installedHash = (Get-FileHash -LiteralPath $registeredDll -Algorithm SHA256).Hash
    if ($buildHash -eq $installedHash) {
        $alreadyRegistered = $true
    } elseif ($build.LastWriteTimeUtc -lt $installed.LastWriteTimeUtc) {
        throw "Build DLL is older than the registered DLL. build='$($build.FullName)' $($build.LastWriteTime); registered='$registeredDll' $($installed.LastWriteTime). Run .\build.ps1 and try again."
    } else {
        Write-Host "Updating: build $($build.LastWriteTime) replaces registered $($installed.LastWriteTime)."
    }
}
if ($alreadyRegistered) {
    Write-Host "No update needed: the registered DLL already is this exact build."
    Write-Host "  CLSID: $clsid"
    Write-Host "  DLL:   $registeredDll"
    Write-Host "Rebuild with .\build.ps1 first, or restart the target application if it still loads an older DLL."
    exit 0
}
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
foreach ($langid in @("00000409", "00000411")) {
    $userProfileKey = "HKCU\Software\Microsoft\CTF\TIP\$clsid\LanguageProfile\0x$langid\$profile"
    & $reg delete $userProfileKey /f 2>$null | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Verbose "No legacy per-user profile to remove: $userProfileKey" }
}
$userZhProfileKey = "HKCU\Software\Microsoft\CTF\TIP\$clsid\LanguageProfile\0x00000804\$profile"
& $reg add $userZhProfileKey /v Enable /t REG_DWORD /d 1 /f | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Per-user zh-CN TSF profile enable failed: $userZhProfileKey" }

Write-Host "Installed and verified:"
Write-Host "  CLSID:   $clsid"
Write-Host "  DLL:     $dll"
Write-Host "  Profile: zh-CN ($profile)"
Write-Host "Restarting ctfmon..."
$ctfmon = Join-Path $env:WINDIR "System32\ctfmon.exe"
if (-not (Test-Path $ctfmon)) { throw "ctfmon.exe was not found at '$ctfmon'." }
Get-Process ctfmon -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Process -FilePath $ctfmon
Write-Host "Restart the target application and select 'TypeScript Windows IME'."
