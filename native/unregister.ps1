$ErrorActionPreference = "Stop"

$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $arguments = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $PSCommandPath)
    Start-Process -FilePath "powershell.exe" -ArgumentList $arguments -Verb RunAs -Wait | Out-Null
    exit $LASTEXITCODE
}

$dll = Join-Path $PSScriptRoot "install\TypeScriptWindowsIme-current.dll"
if (-not (Test-Path $dll)) { throw "Installed native DLL not found: $dll" }

$regsvr32 = Join-Path $env:WINDIR "System32\regsvr32.exe"
$process = Start-Process -FilePath $regsvr32 -ArgumentList @('/u', '/s', $dll) -Wait -PassThru
if ($process.ExitCode -ne 0) { throw "regsvr32 /u failed with exit code $($process.ExitCode)." }

Write-Host "Unregistered TypeScript Windows IME: $dll"
