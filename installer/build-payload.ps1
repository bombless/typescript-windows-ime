$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$nativeBuild = Join-Path $root 'native\build.ps1'
& $nativeBuild -Flavor Release
if ($LASTEXITCODE -ne 0) { throw 'Native Release build failed; refusing to package a Dev DLL as Release.' }
$payload = Join-Path $root 'artifacts\payload'
if (Test-Path $payload) { Remove-Item $payload -Recurse -Force }
New-Item -ItemType Directory -Force -Path $payload | Out-Null
foreach ($file in @('package.json','package-lock.json','tsconfig.json')) { Copy-Item (Join-Path $root $file) $payload }
Copy-Item (Join-Path $root 'src') (Join-Path $payload 'src') -Recurse
if (Test-Path (Join-Path $root 'data')) { Copy-Item (Join-Path $root 'data') (Join-Path $payload 'data') -Recurse }
Copy-Item $PSScriptRoot (Join-Path $payload 'installer') -Recurse
New-Item -ItemType Directory -Force -Path (Join-Path $payload 'native\install') | Out-Null
# Package the artifacts produced by the current build, not the stable files in
# native\install. Those files can intentionally remain old while TSF is still
# using a versioned DLL, which otherwise silently ships stale candidate-window
# painting and Host behavior in a newly built MSI.
$builtDll = Join-Path $root 'native\build\release\TypeScriptWindowsIme.dll'
$builtHost = Join-Path $root 'native\build\TypeScriptWindowsImeHost.exe'
foreach ($artifact in @($builtDll, $builtHost)) {
    if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
        throw "Current native build artifact is missing: $artifact. Run native\build.ps1 before building the MSI."
    }
}
Copy-Item $builtDll (Join-Path $payload 'native\install\TypeScriptWindowsIme.dll') -Force -ErrorAction Stop
Copy-Item $builtHost (Join-Path $payload 'native\install\TypeScriptWindowsImeHost.exe') -Force -ErrorAction Stop
foreach ($pair in @(
    @{ Source = $builtDll; Packaged = (Join-Path $payload 'native\install\TypeScriptWindowsIme.dll') },
    @{ Source = $builtHost; Packaged = (Join-Path $payload 'native\install\TypeScriptWindowsImeHost.exe') }
)) {
    $sourceHash = (Get-FileHash -LiteralPath $pair.Source -Algorithm SHA256).Hash
    $packagedHash = (Get-FileHash -LiteralPath $pair.Packaged -Algorithm SHA256).Hash
    if ($sourceHash -ne $packagedHash) {
        throw "MSI payload verification failed: '$($pair.Packaged)' does not match current build '$($pair.Source)'."
    }
}
Push-Location $payload
try { npm ci; if ($LASTEXITCODE -ne 0) { throw 'npm ci --omit=dev failed' } } finally { Pop-Location }
