[CmdletBinding()]
param(
    # Optional override in MSI's native three-field format, e.g. 26.10.22616.
    [string]$ProductVersion,

    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ProjectRoot = $PSScriptRoot
$NativeBuildScript = Join-Path $ProjectRoot 'native\build.ps1'
$PayloadBuildScript = Join-Path $ProjectRoot 'installer\build-payload.ps1'
$WxsPath = Join-Path $ProjectRoot 'installer\Product.wxs'
$PayloadPath = Join-Path $ProjectRoot 'artifacts\payload'
$ArtifactsPath = Join-Path $ProjectRoot 'artifacts'

# Keep the MSI filename readable as yyyy.MMddHHmm, while encoding a valid,
# chronologically increasing timestamp in MSI's constrained version fields.
# MSI limits: major < 256, minor < 256, build < 65536.
if ([string]::IsNullOrWhiteSpace($ProductVersion)) {
    $now = Get-Date
    $year = [int]$now.ToString('yy')
    $month = [int]$now.ToString('MM')
    # Minutes elapsed since the start of this month: max 44,639 for a 31-day month.
    $minutesIntoMonth = (($now.Day - 1) * 1440) + ($now.Hour * 60) + $now.Minute
    $ProductVersion = '{0}.{1}.{2}' -f $year, $month, $minutesIntoMonth
    $fileVersion = $now.ToString('yyyy.MMddHHmm')
} else {
    if ($ProductVersion -notmatch '^(\d+)\.(\d+)\.(\d+)$') {
        throw 'ProductVersion override must have three numeric fields, e.g. 26.10.22616.'
    }
    $fields = @($ProductVersion.Split('.') | ForEach-Object { [int]$_ })
    if ($fields[0] -lt 0 -or $fields[0] -ge 256 -or
        $fields[1] -lt 0 -or $fields[1] -ge 256 -or
        $fields[2] -lt 0 -or $fields[2] -ge 65536) {
        throw "MSI ProductVersion limits are major < 256, minor < 256, build < 65536: $ProductVersion"
    }
    $fileVersion = $ProductVersion
}

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $ArtifactsPath "TypeScriptWindowsIme-$fileVersion.msi"
} elseif (-not [System.IO.Path]::IsPathRooted($OutputPath)) {
    $OutputPath = Join-Path $ProjectRoot $OutputPath
}

foreach ($requiredPath in @($NativeBuildScript, $PayloadBuildScript, $WxsPath)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Required file not found: $requiredPath"
    }
}

$wixCommand = Get-Command 'wix' -ErrorAction SilentlyContinue
if (-not $wixCommand) {
    throw 'WiX CLI was not found on PATH. Install WiX Toolset CLI 6.x and retry.'
}

Write-Host "==> MSI product version: $ProductVersion"
Write-Host '==> Building native DLL and host executable...'
& $NativeBuildScript
if ($LASTEXITCODE -ne 0 -and $null -ne $LASTEXITCODE) {
    throw "Native build failed with exit code $LASTEXITCODE"
}

Write-Host '==> Building installer payload from the latest native build...'
& $PayloadBuildScript
if ($LASTEXITCODE -ne 0 -and $null -ne $LASTEXITCODE) {
    throw "Payload build failed with exit code $LASTEXITCODE"
}

$payloadDll = Join-Path $PayloadPath 'native\install\TypeScriptWindowsIme.dll'
$payloadHost = Join-Path $PayloadPath 'native\install\TypeScriptWindowsImeHost.exe'
foreach ($requiredPath in @($payloadDll, $payloadHost)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Expected payload file not found: $requiredPath"
    }
}

New-Item -ItemType Directory -Path (Split-Path -Parent $OutputPath) -Force | Out-Null
Write-Host '==> Building MSI...'
& $wixCommand.Source build $WxsPath -arch x64 '-d' "ProductVersion=$ProductVersion" '-d' "PayloadDir=$PayloadPath" -o $OutputPath
if ($LASTEXITCODE -ne 0) {
    throw "WiX MSI build failed with exit code $LASTEXITCODE"
}

if (-not (Test-Path -LiteralPath $OutputPath -PathType Leaf)) {
    throw "WiX reported success but the MSI was not found: $OutputPath"
}

Write-Host "`nMSI created successfully: $OutputPath"
Get-Item -LiteralPath $OutputPath | Select-Object FullName, Length, LastWriteTime
