$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$destination = Join-Path $root 'data\wanxiang-lts-zh-hans.gram'
$url = 'https://github.com/amzxyz/rime-wanxiang-data/releases/download/LTS/wanxiang-lts-zh-hans.gram'
$expectedSize = 418078764
$expectedSha256 = 'e34500ab44626dc039a0641792ecba98ab63307646eb3d29186be56608ffe094'

if (Test-Path -LiteralPath $destination -PathType Leaf) {
    $existing = Get-Item -LiteralPath $destination
    if ($existing.Length -eq $expectedSize -and
        (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant() -eq $expectedSha256) {
        Write-Host "Wanxiang grammar model is already present: $destination"
        exit 0
    }
    throw "An existing model file failed the official size or SHA-256 check: $destination"
}

Write-Host 'Downloading the official 418 MB Wanxiang LTS grammar model...'
$ProgressPreference = 'SilentlyContinue'
try {
    Invoke-WebRequest -Uri $url -OutFile $destination -UseBasicParsing
}
catch {
    if (Test-Path -LiteralPath $destination) {
        Remove-Item -LiteralPath $destination -Force
    }
    throw
}

$downloaded = Get-Item -LiteralPath $destination
$actualSha256 = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
if ($downloaded.Length -ne $expectedSize -or $actualSha256 -ne $expectedSha256) {
    Remove-Item -LiteralPath $destination -Force
    throw "Downloaded model failed verification. Size=$($downloaded.Length), SHA256=$actualSha256"
}

Write-Host "Verified Wanxiang grammar model: $destination"
