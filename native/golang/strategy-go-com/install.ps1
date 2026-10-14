[CmdletBinding()]
param(
    [switch]$Install,
    [switch]$BuildOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Strategy A has a separate identity from the existing Go/C++ TIPs.
$serviceName = 'strategy-go-com'
$clsid = '{A4C7D921-6B35-4E8A-91F2-7D0B6C3E5A14}'
$profileGuid = '{E8B1C4D7-2A63-49F0-B5D8-1C7E3A9F6240}'
$languageIds = @('00000409', '00000804', '00000411')
$dllName = 'TypeScriptWindowsImeGoCom.dll'
$dllPath = Join-Path $PSScriptRoot $dllName
$smokePath = Join-Path $PSScriptRoot 'GoComSmoke.exe'

Write-Host "Input method name: $serviceName"
Write-Host "CLSID:       $clsid"
Write-Host "Profile GUID: $profileGuid"
Write-Host "Languages:   $($languageIds -join ', ')"
Write-Host "DLL:         $dllPath"

if ($clsid -in @('{5D8C1D62-9F73-4B4C-A3D2-1E7B8A9C6401}', '{7B2E4F5A-3A8E-4D74-9F0B-6D5D6F0E6C41}') -or
    $profileGuid -in @('{D47E2A19-6C85-4D31-9B70-3F2A8C5E6102}', '{C0A3B5B1-3C52-4E8E-A8A7-1F2B3D4C5E60}')) {
    throw 'Identity collision with an existing Go/C++ TIP; refusing to continue.'
}
if (-not (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'go.mod'))) {
    throw "go.mod not found in strategy directory: $PSScriptRoot"
}

if (-not $BuildOnly) {
    Write-Warning 'No Windows registration or input-method settings have been changed.'
    Write-Warning 'Real TSF activation is not verified; this prototype is not yet a usable IME.'
}

# Build stays inside this strategy directory and does not register the DLL.
$oldCgoLdfLags = $env:CGO_LDFLAGS
try {
    $env:CGO_LDFLAGS = '-lole32 -luuid'
    Push-Location $PSScriptRoot
    try {
        & go build -buildmode=c-shared -o $dllName .
        if ($LASTEXITCODE -ne 0) { throw "Go DLL build failed with exit code $LASTEXITCODE." }
    }
    finally { Pop-Location }
}
finally { $env:CGO_LDFLAGS = $oldCgoLdfLags }

if (-not (Test-Path -LiteralPath $dllPath)) { throw "Build did not produce expected DLL: $dllPath" }
Get-FileHash -Algorithm SHA256 -LiteralPath $dllPath | Format-List | Out-Host

# Keep the native harness CLSID in sync with main.go; never run a stale harness
# after changing the COM identity.
$clang = Get-Command 'clang++' -ErrorAction SilentlyContinue
if ($clang) {
    Push-Location $PSScriptRoot
    try {
        & $clang.Source '-std=c++17' '-municode' '.\smoke.cpp' '-o' '.\GoComSmoke.exe' '-lole32' '-luuid'
        if ($LASTEXITCODE -ne 0) { throw "Native smoke harness build failed with exit code $LASTEXITCODE." }
    }
    finally { Pop-Location }
}
else {
    Write-Warning 'clang++ not found; the existing smoke harness may be stale and will not be run automatically.'
}

if ($clang -and (Test-Path -LiteralPath $smokePath)) {
    & $smokePath $dllPath
    if ($LASTEXITCODE -ne 0) { throw "Direct COM smoke test failed with exit code $LASTEXITCODE." }
}
elseif (-not $clang -and (Test-Path -LiteralPath $smokePath)) {
    Write-Warning 'Skipped smoke run because clang++ was unavailable to rebuild the harness for the new CLSID.'
}
else {
    Write-Warning "Smoke harness not found; skipped direct COM smoke: $smokePath"
}

if ($BuildOnly) {
    Write-Host 'Build-only mode complete. No registration was attempted.'
    exit 0
}
if (-not $Install) {
    Write-Host 'Plan/build mode complete. No registration was attempted.'
    Write-Host 'Use -Install only after the separate VM and registration review is approved.'
    exit 0
}

# Fail closed until the registration implementation and rollback are reviewed in an approved disposable VM.
throw @'
Installation blocked safely. COM/TSF registration exports are implemented but have not
been exercised against a disposable Windows VM or reviewed for mutation/rollback behavior.
Do not bypass this gate with manual registry edits or regsvr32. No registry values or TSF
profiles were changed by this script. The activation callback is still a stub, so this
prototype cannot yet provide text input even if registration succeeds.
'@