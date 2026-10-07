$ErrorActionPreference = "Stop"
$dll = Join-Path $PSScriptRoot "build\TypeScriptWindowsIme.dll"
if (-not (Test-Path $dll)) { throw "Build the native DLL first: .\build.ps1" }
$regsvr32 = Join-Path $env:WINDIR "System32\regsvr32.exe"
$process = Start-Process -FilePath $regsvr32 -ArgumentList @('/u', '/s', $dll) -Wait -PassThru
if ($process.ExitCode -ne 0) { throw "regsvr32 /u failed with exit code $($process.ExitCode). Run this script elevated." }
Write-Host "Unregistered TypeScript Windows IME: $dll"
