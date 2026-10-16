$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dll = Join-Path $root 'native\install\TypeScriptWindowsIme.dll'
$logFile = Join-Path $env:TEMP 'typescript-windows-ime-installer.log'

function Write-InstallerLog([string]$message) {
    $timestamp = (Get-Date).ToString("yyyy-MM-dd HH:mm:ss")
    $entry = "[$timestamp] [register-tsf] $message"
    try {
        Add-Content -LiteralPath $logFile -Value $entry -Encoding utf8 -ErrorAction SilentlyContinue
    } catch {}
    Write-Host $entry
}

Write-InstallerLog "Starting TSF registration for TypeScript Windows IME Release..."

$clsid = '{A6C63D91-DF29-4D11-8F9A-21DCE63B4E10}'
$profile = '{F2E94A73-8D2B-4F6C-AE31-5D7C903B12A8}'
$displayName = 'TypeScript Windows IME Release'
$system32 = Join-Path $env:WINDIR 'System32'
$reg = Join-Path $system32 'reg.exe'
$regsvr32 = Join-Path $system32 'regsvr32.exe'

# 1. Terminate running host processes that might lock DLL or pipe bridge
Write-InstallerLog "Stopping any running host or test processes..."
Get-Process -Name 'TypeScriptWindowsImeHost', 'PipeBridgeTest' -ErrorAction SilentlyContinue | ForEach-Object {
    Write-InstallerLog "Stopping process $($_.ProcessName) (PID $($_.Id))..."
    Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
}

# 2. Check for leftover/stale DLL registration pointing to a different location or stale installation
$oldRegisteredDll = $null
$inprocKey = "HKLM\SOFTWARE\Classes\CLSID\$clsid\InprocServer32"
try {
    $queryOutput = & $reg query $inprocKey /ve 2>$null
    if ($LASTEXITCODE -eq 0) {
        foreach ($line in $queryOutput) {
            if ($line -match "REG_SZ\s+(.*)$") {
                $oldRegisteredDll = $matches[1].Trim()
                break
            }
        }
    }
} catch {}

if ($oldRegisteredDll) {
    Write-InstallerLog "Found existing registered DLL in registry: $oldRegisteredDll"
    if ($oldRegisteredDll -ne $dll) {
        Write-InstallerLog "Existing DLL path differs from target ($dll). Unregistering old DLL first..."
        if (Test-Path -LiteralPath $oldRegisteredDll) {
            $unregProc = Start-Process -FilePath $regsvr32 -ArgumentList @('/u', '/s', "`"$oldRegisteredDll`"") -Wait -PassThru
            Write-InstallerLog "Old DLL unregister returned: $($unregProc.ExitCode)"
        } else {
            Write-InstallerLog "Old DLL path does not exist on disk, cleaning stale CLSID and TIP registry keys..."
            & $reg delete "HKLM\SOFTWARE\Classes\CLSID\$clsid" /f 2>$null | Out-Null
            & $reg delete "HKLM\SOFTWARE\Microsoft\CTF\TIP\$clsid" /f 2>$null | Out-Null
        }
    }
}

# 3. Ensure target DLL exists
if (-not (Test-Path -LiteralPath $dll)) {
    Write-InstallerLog "ERROR: Target TSF DLL not found at '$dll'"
    throw "TSF DLL not found: $dll"
}

# 4. Register current DLL
Write-InstallerLog "Registering TSF DLL with regsvr32: $dll"
$p = Start-Process -FilePath $regsvr32 -ArgumentList @('/s', "`"$dll`"") -Wait -PassThru
if ($p.ExitCode -ne 0) {
    Write-InstallerLog "ERROR: regsvr32 failed with exit code $($p.ExitCode)"
    throw "regsvr32 failed with exit code $($p.ExitCode)"
}

# 5. Check and enable TSF language profile
$profileKey = "HKLM\SOFTWARE\Microsoft\CTF\TIP\$clsid\LanguageProfile\0x00000804\$profile"
Write-InstallerLog "Checking TSF profile key: $profileKey"
& $reg query $profileKey 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-InstallerLog "WARNING: Profile key missing after regsvr32, creating manually..."
    & $reg add $profileKey /v Description /t REG_SZ /d $displayName /f 2>$null | Out-Null
}

Write-InstallerLog "Enabling TSF language profile..."
& $reg add $profileKey /v Enable /t REG_DWORD /d 1 /f | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-InstallerLog "ERROR: Failed to enable TSF language profile"
    throw 'Failed to enable TSF language profile.'
}

Write-InstallerLog "TSF registration completed successfully."
