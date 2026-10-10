param(
    [switch]$RestartCtfmon
)

$ErrorActionPreference = "Stop"

$clsid = "{7B2E4F5A-3A8E-4D74-9F0B-6D5D6F0E6C41}"
$goClsid = "{5D8C1D62-9F73-4B4C-A3D2-1E7B8A9C6401}"
$strategyClsid = "{B18F4C27-9A63-4E15-8D70-2F6C1A5B9E34}"
$projectRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $PSScriptRoot "build"
$installDir = Join-Path $PSScriptRoot "install"

$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    # Preserve this script's registry-view behavior under elevation, even when
    # the current shell is 32-bit. Capture and return the elevated process code.
    $elevatedShell = if ([Environment]::Is64BitOperatingSystem) {
        Join-Path $env:WINDIR "SysNative\WindowsPowerShell\v1.0\powershell.exe"
    } else {
        Join-Path $env:WINDIR "System32\WindowsPowerShell\v1.0\powershell.exe"
    }
    if (-not (Test-Path -LiteralPath $elevatedShell)) { $elevatedShell = "powershell.exe" }
    $arguments = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $PSCommandPath)
    $child = Start-Process -FilePath $elevatedShell -ArgumentList $arguments -Verb RunAs -Wait -PassThru
    exit $child.ExitCode
}

$system32 = Join-Path $env:WINDIR "System32"
$sysNative = Join-Path $env:WINDIR "SysNative"
$sysWow64 = Join-Path $env:WINDIR "SysWOW64"

# Use explicit registry views instead of relying on process bitness. Some of
# the installers have been launched from 32-bit PowerShell and wrote CLSIDs
# into Registry32, while TSF itself is 64-bit. Cleanup must cover both views.
$nativeSystem32 = if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
    $sysNative
} else {
    $system32
}
$reg = Join-Path $nativeSystem32 "reg.exe"
$regsvr32 = Join-Path $nativeSystem32 "regsvr32.exe"
$reg32 = if ([Environment]::Is64BitOperatingSystem) { Join-Path $sysWow64 "reg.exe" } else { $reg }
$regsvr32_32 = if ([Environment]::Is64BitOperatingSystem) { Join-Path $sysWow64 "regsvr32.exe" } else { $regsvr32 }
$registryViews = if ([Environment]::Is64BitOperatingSystem) {
    @([pscustomobject]@{ Name = '64-bit'; Reg = $reg; Regsvr32 = $regsvr32; Switch = '/reg:64' }, [pscustomobject]@{ Name = '32-bit'; Reg = $reg32; Regsvr32 = $regsvr32_32; Switch = '/reg:32' })
} else {
    @([pscustomobject]@{ Name = 'native'; Reg = $reg; Regsvr32 = $regsvr32; Switch = $null })
}

function Get-RegisteredDllPath([string]$root, [string]$classId, [string]$viewName) {
    $path = if ($root -eq "HKCU") {
        "HKCU\Software\Classes\CLSID\$classId\InprocServer32"
    } else {
        "HKLM\SOFTWARE\Classes\CLSID\$classId\InprocServer32"
    }
    $view = $registryViews | Where-Object { $_.Name -eq $viewName } | Select-Object -First 1
    if (-not $view) { return $null }
    $args = @('query', $path, '/ve')
    if ($view.Switch) { $args += $view.Switch }
    $output = & $view.Reg @args 2>$null
    if ($LASTEXITCODE -ne 0) { return $null }

    foreach ($line in $output) {
        # The value name is localized, but the registry type token is stable.
        if ($line -match "REG_SZ\s+(.*)$") { return $matches[1].Trim() }
    }
    return $null
}

function UnregisterDll([string]$dll, [string]$viewName) {
    if (-not $dll -or -not (Test-Path -LiteralPath $dll)) { return }
    $view = $registryViews | Where-Object { $_.Name -eq $viewName } | Select-Object -First 1
    if (-not $view) { throw "Unknown registry view '$viewName'." }

    Write-Host "Unregistering ($viewName view): $dll"
    # Go adapters can terminate regsvr32 abnormally after DllUnregisterServer.
    # Try the normal path but do not trust its exit code as proof of cleanup;
    # explicit CLSID/CTF key deletion and verification below are authoritative.
    # The installed DLLs are x64, so always use native-architecture regsvr32,
    # even when the CLSID was accidentally written into the 32-bit registry view.
    $p = Start-Process -FilePath $regsvr32 -ArgumentList @('/u', '/s', $dll) -Wait -PassThru
    if ($p.ExitCode -ne 0) {
        Write-Warning "regsvr32 /u returned $($p.ExitCode) for $dll ($viewName view); continuing with explicit registry cleanup."
    }
}

function Remove-RegistryKey([string]$Path, [string]$ViewName) {
    $view = $registryViews | Where-Object { $_.Name -eq $ViewName } | Select-Object -First 1
    if (-not $view) { throw "Unknown registry view '$ViewName'." }
    $args = @('delete', $Path, '/f')
    if ($view.Switch) { $args += $view.Switch }
    $output = & $view.Reg @args 2>$null
    $exitCode = $LASTEXITCODE
    if ($exitCode -eq 0) {
        Write-Host "Deleted ($ViewName view): $Path"
        return
    }
    # Missing keys are expected; verify with query before deciding this failed.
    $queryArgs = @('query', $Path)
    if ($view.Switch) { $queryArgs += $view.Switch }
    & $view.Reg @queryArgs 2>$null | Out-Null
    if ($LASTEXITCODE -eq 0) {
        throw "Failed to delete registry key ($ViewName view): $Path. $($output -join ' ')"
    }
    Write-Host "Already absent ($ViewName view): $Path"
}

function Stop-HostProcess {
    Write-Host "Stopping npm run host, the native TSF relay, and PipeBridgeTest..."
    Get-Process -Name "TypeScriptWindowsImeHost", "PipeBridgeTest" -ErrorAction SilentlyContinue |
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

$registeredDlls = @()
foreach ($view in $registryViews) {
    foreach ($classId in @($clsid, $goClsid, $strategyClsid)) {
        foreach ($root in @('HKCU', 'HKLM')) {
            $path = Get-RegisteredDllPath $root $classId $view.Name
            if ($path) {
                $registeredDlls += [pscustomobject]@{ Path = $path; View = $view.Name; CLSID = $classId; Root = $root }
            }
        }
    }
}
$registeredPaths = @($registeredDlls | Select-Object -ExpandProperty Path -Unique)

$knownDlls = @(
    (Join-Path $buildDir "TypeScriptWindowsIme.dll"),
    (Join-Path $buildDir "TypeScriptWindowsIme.Active.dll"),
    (Join-Path $installDir "TypeScriptWindowsIme-current.dll"),
    (Join-Path $installDir "TypeScriptWindowsIme.dll"),
    (Join-Path $buildDir "golang\TypeScriptWindowsImeGo.dll"),
    (Join-Path $buildDir "golang\TypeScriptWindowsImeGoCore.dll"),
    (Join-Path $installDir "TypeScriptWindowsImeGo-current.dll"),
    (Join-Path $installDir "TypeScriptWindowsImeGoCore.dll"),
    (Join-Path $PSScriptRoot "golang\strategy-generated-bindings\build\StrategyGeneratedBindings.dll"),
    (Join-Path $PSScriptRoot "golang\strategy-generated-bindings\install\StrategyGeneratedBindings-current.dll")
)

foreach ($dll in @($registeredPaths + $knownDlls) | Where-Object { $_ } | Select-Object -Unique) {
    # Every discovered DLL gets one unregister attempt. The CLSID key itself
    # is removed explicitly in each view below, whether this export succeeds or not.
    $dllRecord = $registeredDlls | Where-Object { $_.Path -eq $dll } | Select-Object -First 1
    $viewName = if ($dllRecord) { $dllRecord.View } else { $registryViews[0].Name }
    UnregisterDll $dll $viewName
}

$regKeys = @()
foreach ($view in $registryViews) {
    $regKeys += [pscustomobject]@{ Path = "HKCU\Software\Classes\CLSID\$clsid"; View = $view.Name }
    $regKeys += [pscustomobject]@{ Path = "HKLM\SOFTWARE\Classes\CLSID\$clsid"; View = $view.Name }
    $regKeys += [pscustomobject]@{ Path = "HKCU\Software\Classes\CLSID\$goClsid"; View = $view.Name }
    $regKeys += [pscustomobject]@{ Path = "HKLM\SOFTWARE\Classes\CLSID\$goClsid"; View = $view.Name }
    $regKeys += [pscustomobject]@{ Path = "HKCU\Software\Classes\CLSID\$strategyClsid"; View = $view.Name }
    $regKeys += [pscustomobject]@{ Path = "HKLM\SOFTWARE\Classes\CLSID\$strategyClsid"; View = $view.Name }
    # TSF profile registrations are separate from COM CLSID registration.
    $regKeys += [pscustomobject]@{ Path = "HKLM\SOFTWARE\Microsoft\CTF\TIP\$clsid"; View = $view.Name }
    $regKeys += [pscustomobject]@{ Path = "HKCU\Software\Microsoft\CTF\TIP\$clsid"; View = $view.Name }
    $regKeys += [pscustomobject]@{ Path = "HKLM\SOFTWARE\Microsoft\CTF\TIP\$goClsid"; View = $view.Name }
    $regKeys += [pscustomobject]@{ Path = "HKCU\Software\Microsoft\CTF\TIP\$goClsid"; View = $view.Name }
    $regKeys += [pscustomobject]@{ Path = "HKLM\SOFTWARE\Microsoft\CTF\TIP\$strategyClsid"; View = $view.Name }
    $regKeys += [pscustomobject]@{ Path = "HKCU\Software\Microsoft\CTF\TIP\$strategyClsid"; View = $view.Name }
}

foreach ($entry in $regKeys) {
    Remove-RegistryKey $entry.Path $entry.View
}

# Final verification: do not print success if any COM or TSF registration remains.
$remaining = @()
foreach ($view in $registryViews) {
    foreach ($path in @(
        "HKCU\Software\Classes\CLSID\$clsid", "HKLM\SOFTWARE\Classes\CLSID\$clsid",
        "HKCU\Software\Classes\CLSID\$goClsid", "HKLM\SOFTWARE\Classes\CLSID\$goClsid",
        "HKCU\Software\Classes\CLSID\$strategyClsid", "HKLM\SOFTWARE\Classes\CLSID\$strategyClsid",
        "HKLM\SOFTWARE\Microsoft\CTF\TIP\$clsid", "HKCU\Software\Microsoft\CTF\TIP\$clsid",
        "HKLM\SOFTWARE\Microsoft\CTF\TIP\$goClsid", "HKCU\Software\Microsoft\CTF\TIP\$goClsid",
        "HKLM\SOFTWARE\Microsoft\CTF\TIP\$strategyClsid", "HKCU\Software\Microsoft\CTF\TIP\$strategyClsid"
    )) {
        $queryArgs = @('query', $path)
        if ($view.Switch) { $queryArgs += $view.Switch }
        & $view.Reg @queryArgs 2>$null | Out-Null
        if ($LASTEXITCODE -eq 0) { $remaining += "$($view.Name): $path" }
    }
}
if ($remaining.Count -gt 0) {
    throw "Cleanup incomplete; the following registration keys still exist:`n$($remaining -join "`n")"
}

Write-Host "COM and TSF registrations verified absent for classic CLSID $clsid and Go CLSID $goClsid in all registry views checked."
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
Write-Host "Next: run .\build.ps1, then .\install-user.ps1."
