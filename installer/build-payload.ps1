$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$payload = Join-Path $root 'artifacts\payload'
if (Test-Path $payload) { Remove-Item $payload -Recurse -Force }
New-Item -ItemType Directory -Force -Path $payload | Out-Null
foreach ($file in @('package.json','package-lock.json','tsconfig.json')) { Copy-Item (Join-Path $root $file) $payload }
Copy-Item (Join-Path $root 'src') (Join-Path $payload 'src') -Recurse
if (Test-Path (Join-Path $root 'data')) { Copy-Item (Join-Path $root 'data') (Join-Path $payload 'data') -Recurse }
Copy-Item $PSScriptRoot (Join-Path $payload 'installer') -Recurse
New-Item -ItemType Directory -Force -Path (Join-Path $payload 'native\install') | Out-Null
Copy-Item (Join-Path $root 'native\install\TypeScriptWindowsIme.dll') (Join-Path $payload 'native\install\TypeScriptWindowsIme.dll') -ErrorAction Stop
Copy-Item (Join-Path $root 'native\install\TypeScriptWindowsImeHost.exe') (Join-Path $payload 'native\install\TypeScriptWindowsImeHost.exe') -ErrorAction Stop
Push-Location $payload
try { npm ci --omit=dev; if ($LASTEXITCODE -ne 0) { throw 'npm ci --omit=dev failed' } } finally { Pop-Location }
