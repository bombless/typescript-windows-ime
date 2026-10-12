$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dll = Join-Path $root 'native\install\TypeScriptWindowsIme.dll'
if (-not (Test-Path -LiteralPath $dll)) { throw "TSF DLL not found: $dll" }
$regsvr32 = Join-Path $env:WINDIR 'System32\regsvr32.exe'
$p = Start-Process -FilePath $regsvr32 -ArgumentList @('/s', $dll) -Wait -PassThru
if ($p.ExitCode -ne 0) { throw "regsvr32 failed with exit code $($p.ExitCode)" }
$clsid = '{7B2E4F5A-3A8E-4D74-9F0B-6D5D6F0E6C41}'
$profile = '{C0A3B5B1-3C52-4E8E-A8A7-1F2B3D4C5E60}'
$reg = Join-Path $env:WINDIR 'System32\reg.exe'
$profileKey = "HKLM\SOFTWARE\Microsoft\CTF\TIP\$clsid\LanguageProfile\0x00000804\$profile"
& $reg query $profileKey 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) { throw "TSF language profile registration was not created: $profileKey" }
& $reg add $profileKey /v Enable /t REG_DWORD /d 1 /f | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Failed to enable TSF language profile.' }
