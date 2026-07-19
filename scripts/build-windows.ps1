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
  "--preset", "release", "-S", $Root, "-B", $Build,
  "-DCMAKE_BUILD_TYPE=$BuildType",
  "-DCMAKE_PREFIX_PATH=$QtRoot",
  "-DQUIZPANE_ENABLE_TESSERACT_OCR=ON",
  "-DQUIZPANE_PORTABLE_CPU_BASELINE=ON",
  "-DQUIZPANE_ENABLE_DIAGNOSTIC_LOGGING=$DiagnosticLogging",
  "-DQUIZPANE_ENABLE_VERBOSE_DIAGNOSTICS=$VerboseDiagnostics",
  "-DQUIZPANE_BUILD_TESTS=ON"
)
if ($CMakeToolchainFile) { $CMakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$CMakeToolchainFile" }
if ($VcpkgTargetTriplet) { $CMakeArgs += "-DVCPKG_TARGET_TRIPLET=$VcpkgTargetTriplet" }
cmake @CMakeArgs
if ($LASTEXITCODE -ne 0) { throw "CMake 配置失败，退出码 $LASTEXITCODE" }
cmake --build $Build --parallel
if ($LASTEXITCODE -ne 0) { throw "项目编译失败，退出码 $LASTEXITCODE" }
ctest --test-dir $Build --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "自动测试失败，退出码 $LASTEXITCODE" }

$Stage = Join-Path $Dist "QuizPane"
if (Test-Path $Stage) { Remove-Item $Stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $Stage | Out-Null
$Executables = @("小窗刷题.exe", "题库制作器.exe")
$Sources = @(
  (Join-Path $Build "apps/desktop-qt/小窗刷题.exe"),
  (Join-Path $Build "apps/bank-studio/题库制作器.exe")
)
for ($Index = 0; $Index -lt $Executables.Count; $Index++) {
  Copy-Item $Sources[$Index] (Join-Path $Stage $Executables[$Index]) -Force
  if ($DebugBuild) {
    $Pdb = [System.IO.Path]::ChangeExtension($Sources[$Index], ".pdb")
    if (Test-Path $Pdb) { Copy-Item $Pdb $Stage -Force }
  }
  & (Join-Path $QtRoot "bin/windeployqt.exe") --release --no-translations `
    (Join-Path $Stage $Executables[$Index])
  if ($LASTEXITCODE -ne 0) { throw "Qt 运行库部署失败，退出码 $LASTEXITCODE" }
}
Copy-Item (Join-Path $Root "LICENSE") $Stage -Force
if (-not $TessdataDir) { throw "请通过 -TessdataDir 或 TESSDATA_DIR 指定 OCR 语言数据目录" }
$Tessdata = Join-Path $Stage "tessdata"
New-Item -ItemType Directory -Force -Path $Tessdata | Out-Null
foreach ($Language in @("chi_sim", "eng")) {
  $Source = Join-Path $TessdataDir "$Language.traineddata"
  if (-not (Test-Path $Source)) { throw "缺少 OCR 语言数据：$Source" }
  Copy-Item $Source $Tessdata -Force
}
$PortableArchive = Join-Path $Dist "QuizPane-windows-x64-portable$PackageSuffix.zip"
if (Test-Path $PortableArchive) { Remove-Item $PortableArchive -Force }

# 绿色版必须保留完整部署目录：Qt DLL、插件、OCR 语言数据与两个程序缺一不可。
# 直接压缩目录本身，使用户解压后得到一个独立文件夹，而不是散落到下载目录。
Compress-Archive -Path $Stage -DestinationPath $PortableArchive -CompressionLevel Optimal
Write-Host "已生成绿色版：$PortableArchive"
