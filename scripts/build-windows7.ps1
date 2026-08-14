param(
    [string]$QtRoot = $env:QT_ROOT,
    [string]$BuildDir = "build/release-windows7",
    [string]$DistDir = "dist/windows7",
    [switch]$DebugBuild,
    [switch]$EnableDiagnosticLogging,
    [switch]$VerboseLogs
)

$ErrorActionPreference = "Stop"
$BuildScript = Join-Path $PSScriptRoot "build-windows.ps1"
$Arguments = @{
    QtRoot = $QtRoot
    BuildDir = $BuildDir
    DistDir = $DistDir
    QtMajorVersion = "5"
    Windows7Compat = $true
    DisableOcr = $true
    DebugBuild = $DebugBuild
    EnableDiagnosticLogging = $EnableDiagnosticLogging
    VerboseLogs = $VerboseLogs
}

& $BuildScript @Arguments
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
