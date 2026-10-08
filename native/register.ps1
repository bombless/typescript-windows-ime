$ErrorActionPreference = "Stop"

$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $arguments = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $PSCommandPath)
    Start-Process -FilePath "powershell.exe" -ArgumentList $arguments -Verb RunAs -Wait | Out-Null
    exit $LASTEXITCODE
}

$buildDll = Join-Path $PSScriptRoot "build\TypeScriptWindowsIme.dll"
$installDir = Join-Path $PSScriptRoot "install"
$dll = Join-Path $installDir "TypeScriptWindowsIme-current.dll"
if (-not (Test-Path $buildDll)) { throw "Build the native DLL first: .\build.ps1" }
$null = New-Item -ItemType Directory -Force -Path $installDir
Copy-Item -Force $buildDll $dll
$regsvr32 = Join-Path $env:WINDIR "System32\regsvr32.exe"
$process = Start-Process -FilePath $regsvr32 -ArgumentList @('/s', $dll) -Wait -PassThru
if ($process.ExitCode -ne 0) { throw "regsvr32 failed with exit code $($process.ExitCode)." }

Write-Host "Registered TypeScript Windows IME: $dll"
Write-Host "Close and reopen Windows Settings before checking the keyboard list."
