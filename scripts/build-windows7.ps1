param(
    [string]$QtRoot = $env:QT_ROOT,
    [string]$BuildDir = "build/release-windows7",
    [string]$DistDir = "dist/windows7",
    [string]$CMakeToolchainFile = "",
    [string]$VcpkgTargetTriplet = "",
    [string]$TessdataDir = $env:TESSDATA_DIR,
    [switch]$EnableOcr,
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
    DebugBuild = $DebugBuild
    EnableDiagnosticLogging = $EnableDiagnosticLogging
    VerboseLogs = $VerboseLogs
}
if (-not $EnableOcr) { $Arguments.DisableOcr = $true }
if ($CMakeToolchainFile) { $Arguments.CMakeToolchainFile = $CMakeToolchainFile }
if ($VcpkgTargetTriplet) { $Arguments.VcpkgTargetTriplet = $VcpkgTargetTriplet }
if ($TessdataDir) { $Arguments.TessdataDir = $TessdataDir }

& $BuildScript @Arguments
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
