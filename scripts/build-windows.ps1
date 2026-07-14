param(
    [string]$QtRoot = $env:QT_ROOT,
    [string]$BuildDir = "build/release-windows",
    [string]$DistDir = "dist/windows",
    [string]$CMakeToolchainFile = "",
    [string]$VcpkgTargetTriplet = "",
    [string]$TessdataDir = $env:TESSDATA_DIR,
    [switch]$DebugBuild,
    [switch]$VerboseLogs
)
$ErrorActionPreference = "Stop"
if (-not $QtRoot) { throw "请通过 -QtRoot 或 QT_ROOT 指定 Qt 6 的 MSVC 目录" }

$Root = (Resolve-Path "$PSScriptRoot/..").Path
$Build = Join-Path $Root $BuildDir
$Dist = Join-Path $Root $DistDir
$BuildType = if ($DebugBuild) { "RelWithDebInfo" } else { "Release" }
$DiagnosticLogging = if ($DebugBuild) { "ON" } else { "OFF" }
$PackageSuffix = if ($DebugBuild) { "-debug" } else { "" }
if ($VerboseLogs -and -not $DebugBuild) {
  throw "-VerboseLogs 只能与 -DebugBuild 一起使用"
}
$VerboseDiagnostics = if ($VerboseLogs) { "ON" } else { "OFF" }
$CMakeArgs = @(
  "-S", $Root, "-B", $Build, "-G", "Ninja",
  "-DCMAKE_BUILD_TYPE=$BuildType",
  "-DCMAKE_PREFIX_PATH=$QtRoot",
  "-DQUIZPANE_ENABLE_TESSERACT_OCR=ON",
  "-DQUIZPANE_ENABLE_DIAGNOSTIC_LOGGING=$DiagnosticLogging",
  "-DQUIZPANE_ENABLE_VERBOSE_DIAGNOSTICS=$VerboseDiagnostics",
  "-DQUIZPANE_BUILD_TESTS=ON"
)
if ($CMakeToolchainFile) { $CMakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$CMakeToolchainFile" }
if ($VcpkgTargetTriplet) { $CMakeArgs += "-DVCPKG_TARGET_TRIPLET=$VcpkgTargetTriplet" }
cmake @CMakeArgs
cmake --build $Build --parallel
ctest --test-dir $Build --output-on-failure

$Packages = @(
  @{
    Name = "QuizPane"
    Executable = "小窗刷题.exe"
    Source = Join-Path $Build "apps/desktop-qt/小窗刷题.exe"
  },
  @{
    Name = "QuizPane-Question-Maker"
    Executable = "题库制作器.exe"
    Source = Join-Path $Build "apps/bank-studio/题库制作器.exe"
  }
)

foreach ($Package in $Packages) {
  $Stage = Join-Path $Dist $Package.Name
  if (Test-Path $Stage) { Remove-Item $Stage -Recurse -Force }
  New-Item -ItemType Directory -Force -Path $Stage | Out-Null
  Copy-Item $Package.Source (Join-Path $Stage $Package.Executable) -Force
  if ($DebugBuild) {
    $Pdb = [System.IO.Path]::ChangeExtension($Package.Source, ".pdb")
    if (Test-Path $Pdb) { Copy-Item $Pdb $Stage -Force }
  }
  Copy-Item (Join-Path $Root "LICENSE") $Stage -Force
  & (Join-Path $QtRoot "bin/windeployqt.exe") --release --no-translations `
    (Join-Path $Stage $Package.Executable)

  if ($Package.Name -eq "QuizPane-Question-Maker") {
    if (-not $TessdataDir) { throw "请通过 -TessdataDir 或 TESSDATA_DIR 指定 OCR 语言数据目录" }
    $Tessdata = Join-Path $Stage "tessdata"
    New-Item -ItemType Directory -Force -Path $Tessdata | Out-Null
    foreach ($Language in @("chi_sim", "eng")) {
      $Source = Join-Path $TessdataDir "$Language.traineddata"
      if (-not (Test-Path $Source)) { throw "缺少 OCR 语言数据：$Source" }
      Copy-Item $Source $Tessdata -Force
    }
  }

  $Zip = Join-Path $Dist "$($Package.Name)-windows-x64$PackageSuffix.zip"
  if (Test-Path $Zip) { Remove-Item $Zip -Force }
  Compress-Archive -Path "$Stage/*" -DestinationPath $Zip -CompressionLevel Optimal
  $SizeMiB = [math]::Round((Get-Item $Zip).Length / 1MB, 1)
  Write-Host "已生成：$Zip ($SizeMiB MiB)"
}
