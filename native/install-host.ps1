$ErrorActionPreference = "Stop"

# Publishes the Host built by build.ps1 and restarts it.
#
# A running Host keeps its image locked, so the built binary is never replaced
# in place. Like the TSF DLL, every publish gets a unique file under
# native\install and the previous instance is stopped first.
$buildExe = Join-Path $PSScriptRoot "build\TypeScriptWindowsImeHost.exe"
$installDir = Join-Path $PSScriptRoot "install"

if (-not (Test-Path $buildExe)) { throw "Native build Host not found: $buildExe. Run .\build.ps1 first." }

$running = @(Get-Process -Name "TypeScriptWindowsImeHost*" -ErrorAction SilentlyContinue)
foreach ($process in $running) {
    Write-Host "Stopping running Host pid=$($process.Id)"
    $process | Stop-Process -Force -ErrorAction SilentlyContinue
}
foreach ($process in $running) {
    try { $process.WaitForExit(5000) | Out-Null } catch {}
}

New-Item -ItemType Directory -Force -Path $installDir | Out-Null
$version = Get-Date -Format "yyyyMMdd-HHmmssfff"
$exe = Join-Path $installDir "TypeScriptWindowsImeHost-$version.exe"
Copy-Item -Force $buildExe $exe -ErrorAction Stop

Write-Host "Host published:"
Write-Host "  $exe"
Write-Host "Starting it in the background."
Start-Process -FilePath $exe -WindowStyle Minimized
Write-Host "Stop it with: Get-Process -Name 'TypeScriptWindowsImeHost*' | Stop-Process -Force"