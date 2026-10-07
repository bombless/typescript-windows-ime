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
    cl.exe /nologo /std:c++17 /EHsc /W4 /LD TsIme.cpp /link /OUT:"$out\TypeScriptWindowsIme.dll" /IMPLIB:"$out\TypeScriptWindowsIme.lib" /DEF:"TsIme.def" /SUBSYSTEM:WINDOWS advapi32.lib ole32.lib
}
finally { Pop-Location }
