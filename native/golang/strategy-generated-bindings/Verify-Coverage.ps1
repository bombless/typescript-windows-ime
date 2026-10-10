[CmdletBinding()]
param(
    [string]$BindingRoot = (Join-Path (go env GOPATH) 'pkg\mod\github.com\zzl'),
    [string]$SdkInclude = ''
)

$ErrorActionPreference = 'Stop'
$interfaces = [ordered]@{
    'ITfTextInputProcessor' = @('Activate', 'Deactivate')
    'ITfKeyEventSink' = @('OnSetFocus', 'OnTestKeyDown', 'OnTestKeyUp', 'OnKeyDown', 'OnKeyUp', 'OnPreservedKey')
    'ITfKeystrokeMgr' = @('AdviseKeyEventSink', 'UnadviseKeyEventSink')
    'ITfEditSession' = @('DoEditSession')
}

if (-not $SdkInclude) {
    $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Include'
    $latest = Get-ChildItem $kits -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.FullName 'um\msctf.h') } |
        Sort-Object Name -Descending | Select-Object -First 1
    if ($latest) { $SdkInclude = Join-Path $latest.FullName 'um' }
}

Write-Output 'TSF binding coverage discovery (read-only; not an ABI test)'
Write-Output "BindingRoot: $BindingRoot"
Write-Output "SdkInclude:  $SdkInclude"

$sdkHeader = if ($SdkInclude) { Join-Path $SdkInclude 'msctf.h' } else { '' }
if (-not $sdkHeader -or -not (Test-Path $sdkHeader)) {
    Write-Warning 'msctf.h not found. Pass -SdkInclude pointing to a Windows SDK um directory.'
} else {
    Write-Output "SDK header found: $sdkHeader"
    foreach ($name in $interfaces.Keys) {
        $matches = Select-String -Path $sdkHeader -Pattern $name -SimpleMatch | Select-Object -First 1
        if ($matches) { Write-Output ("SDK {0}: present (line {1})" -f $name, $matches.LineNumber) }
        else { Write-Output "SDK ${name}: NOT FOUND" }
    }
}

$bindingFiles = @()
if (Test-Path $BindingRoot) {
    $bindingFiles = @(Get-ChildItem $BindingRoot -Recurse -File -Include '*.go' -ErrorAction SilentlyContinue)
} else {
    Write-Warning 'Binding source root is not present locally; binding coverage remains UNKNOWN.'
}

if ($bindingFiles.Count -gt 0) {
    foreach ($name in $interfaces.Keys) {
        $hits = @($bindingFiles | Select-String -Pattern $name -SimpleMatch -List)
        if ($hits.Count -gt 0) {
            Write-Output "BINDING ${name}: found in $($hits.Count) file(s)"
            $hits | Select-Object -First 5 | ForEach-Object { Write-Output "  $($_.Path)" }
            foreach ($method in $interfaces[$name]) {
                $methodHits = @($bindingFiles | Select-String -Pattern $method -SimpleMatch -List)
                if ($methodHits.Count -gt 0) { Write-Output "  method candidate ${method}: found" }
                else { Write-Output "  method candidate ${method}: NOT FOUND by text search" }
            }
        } else {
            Write-Output "BINDING ${name}: no text match (does not prove absence; generation may omit or rename it)"
        }
    }
} else {
    Write-Output 'BINDING coverage: UNKNOWN (no Go source files available to scan).'
}

Write-Output ''
Write-Output 'Reminder: inspect exact GUIDs, signatures, inherited IUnknown slots, and vtable order manually.'