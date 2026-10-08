$ErrorActionPreference = "Stop"

function Find-VsDevCmd {
    $vswhereCandidates = @(
        (Join-Path ${env:ProgramFiles} "Microsoft Visual Studio\Installer\vswhere.exe"),
        (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe")
    ) | Where-Object { $_ -and (Test-Path $_) }

    foreach ($vswhere in $vswhereCandidates) {
        $installationPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($LASTEXITCODE -eq 0 -and $installationPath) {
            $candidate = Join-Path $installationPath "Common7\Tools\VsDevCmd.bat"
            if (Test-Path $candidate) { return $candidate }
        }
    }

    $knownInstallations = @(
        (Join-Path ${env:ProgramFiles} "Microsoft Visual Studio\2022\Enterprise"),
        (Join-Path ${env:ProgramFiles} "Microsoft Visual Studio\2022\Professional"),
        (Join-Path ${env:ProgramFiles} "Microsoft Visual Studio\2022\Community"),
        (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\2022\Enterprise"),
        (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\2022\Professional"),
        (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\2022\Community")
    ) | Where-Object { $_ }

    foreach ($installationPath in $knownInstallations) {
        $candidate = Join-Path $installationPath "Common7\Tools\VsDevCmd.bat"
        if (Test-Path $candidate) { return $candidate }
    }

    return $null
}

$cl = Get-Command cl.exe -ErrorAction SilentlyContinue
if (-not $cl) {
    $vsDevCmd = Find-VsDevCmd
    if (-not $vsDevCmd) {
        throw "VS2022 with the C++ x64 toolchain was not found. Install Microsoft.VisualStudio.Component.VC.Tools.x86.x64 or run this script from a Visual Studio Developer PowerShell."
    }

    $envDump = & cmd.exe /d /s /c "call `"$vsDevCmd`" -arch=x64 -host_arch=x64 >nul && set"
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to initialize the Visual Studio developer environment from $vsDevCmd."
    }

    foreach ($line in $envDump) {
        if ($line -match "^([^=]+)=(.*)$") {
            [Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
        }
    }

    $cl = Get-Command cl.exe -ErrorAction SilentlyContinue
    if (-not $cl) { throw "Visual Studio developer environment was initialized, but cl.exe was not found on PATH." }
}

$out = Join-Path $PSScriptRoot "build"
New-Item -ItemType Directory -Force -Path $out | Out-Null
Push-Location $PSScriptRoot
try {
    cl.exe /nologo /std:c++17 /EHsc /W4 /c PipeBridge.cpp /Fo:"$out\PipeBridge.obj"
    cl.exe /nologo /std:c++17 /EHsc /W4 PipeBridgeSmoke.cpp "$out\PipeBridge.obj" /Fe:"$out\PipeBridgeSmoke.exe"
    cl.exe /nologo /std:c++17 /EHsc /W4 PipeBridgeTest.cpp "$out\PipeBridge.obj" /Fe:"$out\PipeBridgeTest.exe"

    $dll = Join-Path $out "TypeScriptWindowsIme.dll"
    $staging = Join-Path $out "staging"
    $stagedDll = Join-Path $staging "TypeScriptWindowsIme.dll"
    $stagedLib = Join-Path $staging "TypeScriptWindowsIme.lib"
    $stagedExp = Join-Path $staging "TypeScriptWindowsIme.exp"

    New-Item -ItemType Directory -Force -Path $staging | Out-Null
    Remove-Item $stagedDll, $stagedLib, $stagedExp -Force -ErrorAction SilentlyContinue
    cl.exe /nologo /std:c++17 /EHsc /W4 /LD TsIme.cpp "$out\PipeBridge.obj" /link /OUT:"$stagedDll" /IMPLIB:"$stagedLib" /DEF:"TsIme.def" /SUBSYSTEM:WINDOWS advapi32.lib ole32.lib user32.lib
    if ($LASTEXITCODE -ne 0) { throw "Failed to link TypeScriptWindowsIme.dll." }

    try {
        Remove-Item $dll -Force -ErrorAction Stop
    } catch {
        Remove-Item $stagedDll, $stagedLib, $stagedExp -Force -ErrorAction SilentlyContinue
        throw "Cannot replace $dll. The existing DLL is probably loaded by a TSF host. Disable the TypeScript Windows IME or restart the affected application, then run build.ps1 again. Original error: $($_.Exception.Message)"
    }

    Move-Item $stagedDll $dll -Force
    Move-Item $stagedLib (Join-Path $out "TypeScriptWindowsIme.lib") -Force
    if (Test-Path $stagedExp) { Move-Item $stagedExp (Join-Path $out "TypeScriptWindowsIme.exp") -Force }
}
finally { Pop-Location }
