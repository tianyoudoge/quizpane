param(
    [string]$QtRoot = $env:QT_ROOT,
    [string]$BuildDir = "build/release-windows7",
    [string]$DistDir = "dist/windows7",
    [ValidateSet("x64", "x86")]
    [string]$Architecture = "x64",
    [switch]$EnableOcr,
    [switch]$DebugBuild,
    [switch]$EnableDiagnosticLogging,
    [switch]$VerboseLogs
)

$ErrorActionPreference = "Stop"
$BuildScript = Join-Path $PSScriptRoot "build-windows.ps1"
$OcrPrefix = ""
if ($EnableOcr) {
    # Keep experimental artifacts/build caches separate from the no-OCR release.
    if (-not $PSBoundParameters.ContainsKey("BuildDir")) { $BuildDir = "build/release-windows7-ocr-$Architecture" }
    if (-not $PSBoundParameters.ContainsKey("DistDir")) { $DistDir = "dist/windows7-ocr-$Architecture" }
    $OcrBuild = Join-Path (Resolve-Path "$PSScriptRoot/..").Path "build/windows7-ocr-$Architecture"
    & "$PSScriptRoot/build-windows7-ocr.ps1" -Architecture $Architecture -BuildDir $OcrBuild
    if ($LASTEXITCODE -ne 0) { throw "Win7 OCR 依赖准备失败" }
    $OcrPrefix = Join-Path $OcrBuild "install"
}
$Arguments = @{
    QtRoot = $QtRoot
    BuildDir = $BuildDir
    DistDir = $DistDir
    QtMajorVersion = "5"
    Architecture = $Architecture
    Windows7Compat = $true
    DisableOcr = -not $EnableOcr
    OcrPrefix = $OcrPrefix
    DebugBuild = $DebugBuild
    EnableDiagnosticLogging = $EnableDiagnosticLogging
    VerboseLogs = $VerboseLogs
}
if ($EnableOcr) {
    $Arguments.TessdataDir = Join-Path $OcrPrefix "tessdata"
}

& $BuildScript @Arguments
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
