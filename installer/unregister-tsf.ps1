$ErrorActionPreference = 'SilentlyContinue'
$root = Split-Path -Parent $PSScriptRoot
$dll = Join-Path $root 'native\install\TypeScriptWindowsIme.dll'
$logFile = Join-Path $env:TEMP 'typescript-windows-ime-installer.log'

function Write-InstallerLog([string]$message) {
    $timestamp = (Get-Date).ToString("yyyy-MM-dd HH:mm:ss")
    $entry = "[$timestamp] [unregister-tsf] $message"
    try {
        Add-Content -LiteralPath $logFile -Value $entry -Encoding utf8 -ErrorAction SilentlyContinue
    } catch {}
    Write-Host $entry
}

Write-InstallerLog "Starting TSF unregistration..."

# 1. Terminate running host processes
Write-InstallerLog "Stopping host processes..."
Get-Process -Name 'TypeScriptWindowsImeHost', 'PipeBridgeTest' -ErrorAction SilentlyContinue | ForEach-Object {
    Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
}

$clsid = '{7B2E4F5A-3A8E-4D74-9F0B-6D5D6F0E6C41}'
$system32 = Join-Path $env:WINDIR 'System32'
$reg = Join-Path $system32 'reg.exe'
$regsvr32 = Join-Path $system32 'regsvr32.exe'

# 2. Try regsvr32 /u if DLL is present
if (Test-Path -LiteralPath $dll) {
    Write-InstallerLog "Unregistering DLL: $dll"
    Start-Process -FilePath $regsvr32 -ArgumentList @('/u', '/s', "`"$dll`"") -Wait -ErrorAction SilentlyContinue
}

# 3. Clean up registry keys explicitly in case regsvr32 /u left them behind
Write-InstallerLog "Cleaning registry entries for CLSID $clsid..."
& $reg delete "HKLM\SOFTWARE\Classes\CLSID\$clsid" /f 2>$null | Out-Null
& $reg delete "HKLM\SOFTWARE\Microsoft\CTF\TIP\$clsid" /f 2>$null | Out-Null

Write-InstallerLog "TSF unregistration finished."
