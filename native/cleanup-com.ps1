param(
    [switch]$RestartCtfmon
)

$ErrorActionPreference = "Stop"

$clsid = "{7B2E4F5A-3A8E-4D74-9F0B-6D5D6F0E6C41}"
$projectRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $PSScriptRoot "build"
$installDir = Join-Path $PSScriptRoot "install"

$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $arguments = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $PSCommandPath)
    Start-Process -FilePath "powershell.exe" -ArgumentList $arguments -Verb RunAs -Wait | Out-Null
    exit $LASTEXITCODE
}

$system32 = Join-Path $env:WINDIR "System32"
$sysNative = Join-Path $env:WINDIR "SysNative"

# SysNative is only needed when a 32-bit process must reach the native
# 64-bit system tools. A normal 64-bit PowerShell must use System32 directly.
$nativeSystem32 = if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
    $sysNative
} else {
    $system32
}

$reg = Join-Path $nativeSystem32 "reg.exe"
$regsvr32 = Join-Path $nativeSystem32 "regsvr32.exe"

function Get-RegisteredDllPath([string]$root) {
    $path = if ($root -eq "HKCU") {
        "HKCU\Software\Classes\CLSID\$clsid\InprocServer32"
    } else {
        "HKLM\SOFTWARE\Classes\CLSID\$clsid\InprocServer32"
    }

    $output = & $reg query $path /ve 2>$null
    if ($LASTEXITCODE -ne 0) { return $null }

    foreach ($line in $output) {
        # The value name is localized, but the registry type token is stable.
        if ($line -match "REG_SZ\s+(.*)$") {
            return $matches[1].Trim()
        }
    }
    return $null
}

function UnregisterDll([string]$dll) {
    if (-not $dll -or -not (Test-Path $dll)) { return }

    Write-Host "Unregistering: $dll"
    $p = Start-Process -FilePath $regsvr32 -ArgumentList @("/u", "/s", $dll) -Wait -PassThru
    if ($p.ExitCode -ne 0) {
        Write-Warning "regsvr32 /u returned $($p.ExitCode) for $dll; registry cleanup will continue."
    }
}

function Stop-HostProcess {
    Write-Host "Stopping npm run host and the native TSF relay..."
    Get-Process -Name "TypeScriptWindowsImeHost" -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue

    $projectPattern = [regex]::Escape($projectRoot)
    $wrappers = Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
        Where-Object {
            $_.ProcessId -ne $PID -and $_.CommandLine -and
            $_.CommandLine -match $projectPattern -and
            ($_.CommandLine -match "host" -or $_.CommandLine -match "TypeScriptWindowsImeHost")
        }
    foreach ($wrapper in $wrappers) {
        Stop-Process -Id ([int]$wrapper.ProcessId) -Force -ErrorAction SilentlyContinue
    }
}

Stop-HostProcess

Write-Host "Stopping common TSF hosts so stale in-process COM DLLs can unload..."
"ctfmon", "TextInputHost", "TabTip", "SearchHost", "SearchApp", "RuntimeBroker", "ApplicationFrameHost" |
    ForEach-Object {
        Get-Process -Name $_ -ErrorAction SilentlyContinue |
            Stop-Process -Force -ErrorAction SilentlyContinue
    }

Start-Sleep -Milliseconds 500

$registeredPaths = @(
    (Get-RegisteredDllPath "HKCU"),
    (Get-RegisteredDllPath "HKLM")
) | Where-Object { $_ } | Select-Object -Unique

$knownDlls = @(
    (Join-Path $buildDir "TypeScriptWindowsIme.dll"),
    (Join-Path $buildDir "TypeScriptWindowsIme.Active.dll"),
    (Join-Path $installDir "TypeScriptWindowsIme-current.dll"),
    (Join-Path $installDir "TypeScriptWindowsIme.dll")
)

foreach ($dll in @($registeredPaths + $knownDlls) | Where-Object { $_ } | Select-Object -Unique) {
    UnregisterDll $dll
}

$regKeys = @(
    "HKCU\Software\Classes\CLSID\$clsid",
    "HKLM\SOFTWARE\Classes\CLSID\$clsid"
)

foreach ($key in $regKeys) {
    & $reg delete $key /f 2>$null | Out-Null
}

Write-Host "COM/TSF registration cleaned for $clsid."
if (-not $RestartCtfmon) {
    Write-Host "ctfmon was left stopped so the build can replace any previously loaded DLL."
    Write-Host "Run .\native\build.ps1 now, then start ctfmon or run .\native\install-user.ps1."
    return
}

Write-Host "Starting ctfmon..."
# Launch through System32 rather than SysNative: when this script runs under
# 32-bit PowerShell, Start-Process cannot CreateProcess() a SysNative alias.
# System32 is automatically redirected to the native system directory as needed.
$ctfmon = Join-Path $system32 "ctfmon.exe"
if (-not (Test-Path $ctfmon)) {
    throw "ctfmon.exe not found at '$ctfmon'."
}
Start-Process -FilePath $ctfmon

Write-Host ""
Write-Host "Build and install DLLs are now independent:"
Write-Host "  Build:   $(Join-Path $buildDir 'TypeScriptWindowsIme.dll')"
Write-Host "  Install: $(Join-Path $installDir 'TypeScriptWindowsIme-current.dll')"
Write-Host ""
Write-Host "Next: run .\build.ps1, then .\install-user.ps1 (or .\install.ps1)."
