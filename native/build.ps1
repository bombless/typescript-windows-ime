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

    # Capture the developer environment after VsDevCmd returns.
    $envDump = & cmd.exe /d /s /c "call `"$vsDevCmd`" -arch=x64 -host_arch=x64 && set"
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to initialize the Visual Studio developer environment from $vsDevCmd."
    }

    $capturedEnvironment = @{}
    foreach ($line in $envDump) {
        if ($line -match "^([^=]+)=(.*)$") {
            $capturedEnvironment[$matches[1].ToUpperInvariant()] = [pscustomobject]@{
                Name = $matches[1]
                Value = $matches[2]
            }
        }
    }
    foreach ($entry in $capturedEnvironment.Values | Where-Object { $_.Name -ine 'PATH' }) {
        [Environment]::SetEnvironmentVariable($entry.Name, $entry.Value, "Process")
    }
# cmd.exe preserves the original case of the variable name, so "Path" must
    # match case-insensitively or the toolchain never lands on PATH.
    $pathLine = $envDump | Where-Object { $_ -imatch '^PATH=' } | Select-Object -First 1
    if ($pathLine -match '^(?i:path)=(.*)$') {
        [Environment]::SetEnvironmentVariable('Path', $matches[1], 'Process')
    }

    $cl = Get-Command cl.exe -ErrorAction SilentlyContinue
    if (-not $cl) { throw "Visual Studio developer environment was initialized, but cl.exe was not found on PATH." }
}

$out = Join-Path $PSScriptRoot "build"
$staging = Join-Path $out "staging"
New-Item -ItemType Directory -Force -Path $out | Out-Null
New-Item -ItemType Directory -Force -Path $staging | Out-Null
Push-Location $PSScriptRoot
try {
    cl.exe /nologo /std:c++17 /EHsc /W4 /c PipeBridge.cpp /Fo:"$out\PipeBridge.obj"
    cl.exe /nologo /std:c++17 /EHsc /W4 /c CandidateWindow.cpp /Fo:"$out\CandidateWindow.obj"
    cl.exe /nologo /std:c++17 /EHsc /W4 PipeBridgeSmoke.cpp "$out\PipeBridge.obj" /Fe:"$out\PipeBridgeSmoke.exe"
    cl.exe /nologo /std:c++17 /EHsc /W4 PipeBridgeTest.cpp "$out\PipeBridge.obj" /Fe:"$out\PipeBridgeTest.exe"
    cl.exe /nologo /std:c++17 /EHsc /W4 TsfDiagnose.cpp /Fe:"$out\TsfDiagnose.exe" ole32.lib advapi32.lib
    # Standalone harness for the candidate window: drives CandidateWindow directly
    # and verifies the painted result against a desktop capture, so the window
    # can be debugged without TSF, an application, or the pipe host. It links
    # into staging so a running copy never blocks the build.
    $probeExe = Join-Path $out "CandidateWindowProbe.exe"
    $stagedProbe = Join-Path $staging "CandidateWindowProbe.exe"
    Remove-Item $probeExe, $stagedProbe -Force -ErrorAction SilentlyContinue
    cl.exe /nologo /std:c++17 /EHsc /W4 /utf-8 CandidateWindowProbe.cpp "$out\CandidateWindow.obj" /Fe:"$stagedProbe" /link user32.lib gdi32.lib dwmapi.lib shell32.lib
    if ($LASTEXITCODE -ne 0) { throw "CandidateWindowProbe.exe failed to build. Fix the compiler output above; the candidate window cannot be debugged without it." }
    try {
        Move-Item $stagedProbe $probeExe -Force
    } catch {
        Write-Warning "Cannot replace native\build\CandidateWindowProbe.exe: $($_.Exception.Message). Stop the running probe and build again."
    }
    if (-not (Test-Path $probeExe)) { throw "CandidateWindowProbe.exe was not produced. See the compiler output above." }

    # The Host is linked into the staging directory first, exactly like the
    # DLL. A running Host keeps its image locked, so replacing the file under
    # native\build would fail with LNK1104. install-host.ps1 copies the staged
    # binary into native\install under a unique name and restarts it.
    $hostExe = Join-Path $out "TypeScriptWindowsImeHost.exe"
    $stagedHost = Join-Path $staging "TypeScriptWindowsImeHost.exe"
    Remove-Item $stagedHost -Force -ErrorAction SilentlyContinue
    cl.exe /nologo /std:c++17 /EHsc /W4 TypeScriptWindowsImeHost.cpp /Fe:"$stagedHost"
    if ($LASTEXITCODE -eq 0) {
        try {
            Move-Item $stagedHost $hostExe -Force
        } catch {
            Write-Warning "Cannot replace native\build\TypeScriptWindowsImeHost.exe: $($_.Exception.Message). Run .\install-host.ps1 to publish and restart a fresh copy."
        }
    } else {
        Write-Warning "TypeScriptWindowsImeHost.exe failed to link; the rest of the build continues."
    }

    # Build artifacts stay under native\\build. The installable DLL is copied
    # into native\\install by the installer and is never touched by this script.
    $dll = Join-Path $out "TypeScriptWindowsIme.dll"
    $stagedDll = Join-Path $staging "TypeScriptWindowsIme.dll"
    $stagedLib = Join-Path $staging "TypeScriptWindowsIme.lib"
    $stagedExp = Join-Path $staging "TypeScriptWindowsIme.exp"

    Remove-Item $stagedDll, $stagedLib, $stagedExp -Force -ErrorAction SilentlyContinue
    cl.exe /nologo /std:c++17 /EHsc /W4 /LD TsIme.cpp "$out\CandidateWindow.obj" "$out\PipeBridge.obj" /link /OUT:"$stagedDll" /IMPLIB:"$stagedLib" /DEF:"TsIme.def" /SUBSYSTEM:WINDOWS advapi32.lib ole32.lib oleaut32.lib user32.lib gdi32.lib
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
