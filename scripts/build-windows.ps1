param(
    [string]$QtRoot = $env:QT_ROOT,
    [string]$BuildDir = "build/release-windows",
    [string]$DistDir = "dist/windows",
    [string]$CMakeToolchainFile = "",
    [string]$VcpkgTargetTriplet = "",
    [string]$TessdataDir = $env:TESSDATA_DIR,
    [ValidateSet("5", "6")]
    [string]$QtMajorVersion = "6",
    [ValidateSet("x64", "x86")]
    [string]$Architecture = "x64",
    [switch]$Windows7Compat,
    [switch]$DisableOcr,
    [switch]$DisablePdf,
    [switch]$DebugBuild,
    [switch]$EnableDiagnosticLogging,
    [switch]$VerboseLogs
)
$ErrorActionPreference = "Stop"
if (-not $QtRoot) { throw "请通过 -QtRoot 或 QT_ROOT 指定 Qt 的 MSVC 目录" }
if ($Windows7Compat -and $QtMajorVersion -ne "5") {
  throw "Windows 7 兼容构建必须使用 -QtMajorVersion 5"
}

$Root = (Resolve-Path "$PSScriptRoot/..").Path
$Build = Join-Path $Root $BuildDir
$Dist = Join-Path $Root $DistDir
$BuildType = if ($DebugBuild) { "RelWithDebInfo" } else { "Release" }
$DiagnosticLogging = if ($DebugBuild -or $EnableDiagnosticLogging) { "ON" } else { "OFF" }
$PackageSuffix = if ($DebugBuild) { "-debug" } else { "" }
if ($VerboseLogs -and -not $DebugBuild) {
  throw "-VerboseLogs 只能与 -DebugBuild 一起使用"
}
$VerboseDiagnostics = if ($VerboseLogs) { "ON" } else { "OFF" }
$OcrEnabled = if ($DisableOcr) { "OFF" } else { "ON" }
$PdfEnabled = if ($DisablePdf) { "OFF" } else { "ON" }
$Windows7Enabled = if ($Windows7Compat) { "ON" } else { "OFF" }
$CMakeArgs = @(
  "--preset", "release", "-S", $Root, "-B", $Build,
  "-DCMAKE_BUILD_TYPE=$BuildType",
  "-DCMAKE_PREFIX_PATH=$QtRoot",
  "-DQUIZPANE_QT_MAJOR_VERSION=$QtMajorVersion",
  "-DQUIZPANE_WINDOWS7_COMPAT=$Windows7Enabled",
  "-DQUIZPANE_ENABLE_TESSERACT_OCR=$OcrEnabled",
  "-DQUIZPANE_ENABLE_QT_PDF=$PdfEnabled",
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
# Windows 7 自带的压缩文件夹组件不能可靠解压 ZIP 中的 UTF-8 文件名。
# 仅 Win7 绿色包使用 ASCII 名称；常规 Windows 包继续保留现有产品名。
$Executables = if ($Windows7Compat) {
  @("QuizPane.exe", "QuizPaneStudio.exe")
} else {
  @("小窗刷题.exe", "题库制作器.exe")
}
$Sources = @(
  (Join-Path $Build "apps/desktop-qt/小窗刷题.exe"),
  (Join-Path $Build "apps/bank-studio/题库制作器.exe")
)
for ($Index = 0; $Index -lt $Executables.Count; $Index++) {
  Copy-Item $Sources[$Index] (Join-Path $Stage $Executables[$Index]) -Force
  if ($DebugBuild) {
    $Pdb = [System.IO.Path]::ChangeExtension($Sources[$Index], ".pdb")
    $PackagedPdb = [System.IO.Path]::ChangeExtension($Executables[$Index], ".pdb")
    if (Test-Path $Pdb) { Copy-Item $Pdb (Join-Path $Stage $PackagedPdb) -Force }
  }
  & (Join-Path $QtRoot "bin/windeployqt.exe") --release --no-translations `
    (Join-Path $Stage $Executables[$Index])
  if ($LASTEXITCODE -ne 0) { throw "Qt 运行库部署失败，退出码 $LASTEXITCODE" }
}
Copy-Item (Join-Path $Root "LICENSE") $Stage -Force
# Qt 5.15.2 的 windeployqt 不识别 qtpdf 模块（官方未独立发行），PDF 导入
# 运行库需要手动随包分发；Qt 6 走常规模块自动部署。
if (-not $DisablePdf -and $QtMajorVersion -eq "5") {
  $PdfRuntimeName = if ($DebugBuild) { "Qt5Pdfd.dll" } else { "Qt5Pdf.dll" }
  $PdfRuntime = Join-Path $QtRoot "bin/$PdfRuntimeName"
  if (-not (Test-Path $PdfRuntime)) { throw "缺少 Qt5Pdf 运行库：$PdfRuntime" }
  Copy-Item $PdfRuntime $Stage -Force
}
# Win7 绿色包不能假定系统已经安装 VC++ Redistributable 或 Universal CRT。
# v142 的 MSVCP/VCRUNTIME DLL 和完整 UCRT（含 api-ms-win-crt 转发 DLL）必须与
# EXE 放在同一目录；否则干净的 Win7 SP1 会在进程加载阶段报缺少 MSVCP140.dll。
if ($Windows7Compat) {
  $V142Redist = if ($env:VSINSTALLDIR -and $env:VCToolsVersion) {
    Join-Path $env:VSINSTALLDIR "VC\Redist\MSVC\$($env:VCToolsVersion)\$Architecture\Microsoft.VC142.CRT"
  }
  if (-not $V142Redist -or -not (Test-Path $V142Redist)) {
    $V142Redist = if ($env:VCToolsRedistDir) {
      Join-Path $env:VCToolsRedistDir "$Architecture\Microsoft.VC142.CRT"
    }
  }
  if (-not $V142Redist -or -not (Test-Path $V142Redist)) {
    throw "找不到 MSVC v142 可再发行运行库目录；请在 VS 2019/v142 开发者命令行中构建"
  }
  $UcrtRedist = if ($env:UniversalCRTSdkDir) {
    Join-Path $env:UniversalCRTSdkDir "Redist\ucrt\DLLs\$Architecture"
  }
  if (-not $UcrtRedist -or -not (Test-Path $UcrtRedist)) {
    throw "找不到 Universal CRT 可再发行运行库目录；请安装 Windows SDK"
  }
  Copy-Item (Join-Path $V142Redist "*.dll") $Stage -Force
  Copy-Item (Join-Path $UcrtRedist "*.dll") $Stage -Force
}
if (-not $DisableOcr) {
  if (-not $TessdataDir) { throw "请通过 -TessdataDir 或 TESSDATA_DIR 指定 OCR 语言数据目录" }
  $Tessdata = Join-Path $Stage "tessdata"
  New-Item -ItemType Directory -Force -Path $Tessdata | Out-Null
  foreach ($Language in @("chi_sim", "eng")) {
    $Source = Join-Path $TessdataDir "$Language.traineddata"
    if (-not (Test-Path $Source)) { throw "缺少 OCR 语言数据：$Source" }
    Copy-Item $Source $Tessdata -Force
  }
}
$PlatformName = if ($Windows7Compat) { "windows7-$Architecture" } else { "windows-$Architecture" }
$PortableArchive = Join-Path $Dist "QuizPane-$PlatformName-portable$PackageSuffix.zip"
if (Test-Path $PortableArchive) { Remove-Item $PortableArchive -Force }

# 绿色版必须保留完整部署目录：Qt DLL、插件、OCR 语言数据与两个程序缺一不可。
# 直接压缩目录本身，使用户解压后得到一个独立文件夹，而不是散落到下载目录。
Compress-Archive -Path $Stage -DestinationPath $PortableArchive -CompressionLevel Optimal
Write-Host "已生成绿色版：$PortableArchive"
