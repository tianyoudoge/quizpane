param(
    [ValidateSet("x64", "x86")]
    [string]$Architecture = "x64",
    [string]$OutputDir = ""
)

$ErrorActionPreference = "Stop"
$Version = "1.1.1w"
$ArchiveSha256 = "cf3098950cb4d853ad95c0841f1f9c6d3dc102dccfcacd521d93925208b76ac8"
$Root = (Resolve-Path "$PSScriptRoot/..").Path
$BuildRoot = Join-Path $Root "build/windows7-openssl-$Architecture"
if (-not $OutputDir) { $OutputDir = Join-Path $BuildRoot "runtime" }
elseif (-not [System.IO.Path]::IsPathRooted($OutputDir)) { $OutputDir = Join-Path $Root $OutputDir }

$Suffix = if ($Architecture -eq "x64") { "-x64" } else { "" }
$RuntimeNames = @("libcrypto-1_1$Suffix.dll", "libssl-1_1$Suffix.dll")
$RuntimeReady = $true
foreach ($Name in $RuntimeNames) {
    if (-not (Test-Path (Join-Path $OutputDir $Name))) { $RuntimeReady = $false }
}
if ($RuntimeReady -and (Test-Path (Join-Path $OutputDir "LICENSE.txt"))) {
    Write-Host "Using cached OpenSSL $Version runtime: $OutputDir"
    return
}

foreach ($Tool in @("perl.exe", "nmake.exe", "tar.exe")) {
    if (-not (Get-Command $Tool -ErrorAction SilentlyContinue)) {
        throw "Building the Win7 TLS runtime requires $Tool on PATH"
    }
}

New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null
$Archive = Join-Path $BuildRoot "openssl-$Version.tar.gz"
$SourceDir = Join-Path $BuildRoot "openssl-$Version"
$Url = "https://www.openssl.org/source/old/1.1.1/openssl-$Version.tar.gz"

if (-not (Test-Path $Archive)) {
    Invoke-WebRequest -Uri $Url -OutFile $Archive
}
$ActualSha256 = (Get-FileHash $Archive -Algorithm SHA256).Hash.ToLowerInvariant()
if ($ActualSha256 -ne $ArchiveSha256) {
    throw "OpenSSL source checksum mismatch: expected $ArchiveSha256, got $ActualSha256"
}

if (Test-Path $SourceDir) { Remove-Item $SourceDir -Recurse -Force }
tar -xf $Archive -C $BuildRoot
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $SourceDir)) {
    throw "Failed to extract OpenSSL $Version source"
}

$Target = if ($Architecture -eq "x64") { "VC-WIN64A" } else { "VC-WIN32" }
Push-Location $SourceDir
try {
    # no-asm avoids an extra NASM dependency and keeps the portable Win7
    # runtime on a conservative CPU baseline; TLS throughput is not a bottleneck here.
    perl Configure $Target shared no-asm no-tests no-ssl3 no-comp
    if ($LASTEXITCODE -ne 0) { throw "OpenSSL configure failed" }
    nmake build_libs
    if ($LASTEXITCODE -ne 0) { throw "OpenSSL build failed" }
} finally {
    Pop-Location
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
foreach ($Name in $RuntimeNames) {
    $Source = Join-Path $SourceDir $Name
    if (-not (Test-Path $Source)) { throw "OpenSSL build did not produce $Name" }
    Copy-Item $Source (Join-Path $OutputDir $Name) -Force
}
Copy-Item (Join-Path $SourceDir "LICENSE") (Join-Path $OutputDir "LICENSE.txt") -Force
Write-Host "Prepared OpenSSL $Version Win7 runtime: $OutputDir"
