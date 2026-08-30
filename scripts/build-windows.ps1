param(
    [string]$QtRoot = $env:QT_ROOT,
    [string]$BuildDir = "build/release-windows",
    [string]$DistDir = "dist/windows",
    [string]$CMakeToolchainFile = "",
    [string]$VcpkgTargetTriplet = "",
    [string]$TessdataDir = $env:TESSDATA_DIR,
    [string]$OcrPrefix = "",
    [string]$OpenSslRoot = "",
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
if ($OcrPrefix) {
  $CMakeArgs += "-DTesseract_DIR=$OcrPrefix/lib/cmake/tesseract"
  $CMakeArgs += "-DLeptonica_DIR=$OcrPrefix/lib/cmake/leptonica"
}
# ctest runs before staging the app: pass the same verified models used in the ZIP.
$PreviousTessdata = $env:TESSDATA_DIR
if (-not $DisableOcr -and $TessdataDir) { $env:TESSDATA_DIR = $TessdataDir }
try {
  cmake @CMakeArgs
  if ($LASTEXITCODE -ne 0) { throw "CMake 配置失败，退出码 $LASTEXITCODE" }
  cmake --build $Build --parallel
  if ($LASTEXITCODE -ne 0) { throw "项目编译失败，退出码 $LASTEXITCODE" }
  ctest --test-dir $Build --output-on-failure
  if ($LASTEXITCODE -ne 0) { throw "自动测试失败，退出码 $LASTEXITCODE" }
} finally {
  $env:TESSDATA_DIR = $PreviousTessdata
}

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
  & (Join-Path $QtRoot "bin/windeployqt.exe") --release --no-translations --no-compiler-runtime `
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
# 绿色包不能把构建机已经安装 VC++ Redistributable 当成用户机器的前置条件。
# windeployqt 的 vc_redist 安装程序不会让解压后的 EXE 直接可用，
# 因此所有 Windows 包都把当前架构/工具集的 MSVCP/VCRUNTIME DLL 放在 EXE 同级。
$MsvcRedist = $null
if ($env:VCToolsRedistDir) {
  $MsvcArchitectureRoot = Join-Path $env:VCToolsRedistDir $Architecture
  if (Test-Path $MsvcArchitectureRoot) {
    $MsvcCandidates = @(Get-ChildItem $MsvcArchitectureRoot -Directory -Filter "Microsoft.VC*.CRT" |
        Sort-Object Name -Descending)
    if ($Windows7Compat) {
      $MsvcRedist = $MsvcCandidates | Where-Object Name -eq "Microsoft.VC142.CRT" | Select-Object -First 1
    } else {
      $MsvcRedist = $MsvcCandidates | Select-Object -First 1
    }
  }
}
if (-not $MsvcRedist -and $env:VSINSTALLDIR -and $env:VCToolsVersion) {
  $MsvcFamily = if ($Windows7Compat) { "Microsoft.VC142.CRT" } else { "Microsoft.VC143.CRT" }
  $MsvcPath = Join-Path $env:VSINSTALLDIR "VC\Redist\MSVC\$($env:VCToolsVersion)\$Architecture\$MsvcFamily"
  if (Test-Path $MsvcPath) { $MsvcRedist = Get-Item $MsvcPath }
}
if (-not $MsvcRedist) {
  throw "找不到当前 $Architecture MSVC 可再发行运行库目录；请在匹配工具集的开发者命令行中构建"
}
Copy-Item (Join-Path $MsvcRedist.FullName "*.dll") $Stage -Force
foreach ($RuntimeName in @("MSVCP140.dll", "VCRUNTIME140.dll")) {
  if (-not (Test-Path (Join-Path $Stage $RuntimeName))) {
    throw "绿色包缺少 MSVC 运行库：$RuntimeName"
  }
}

# Win7 还不能假定系统已经安装 Universal CRT。完整 UCRT（含 api-ms-win-crt
# 转发 DLL）必须与 EXE 放在同一目录。
if ($Windows7Compat) {
  $UcrtRedist = if ($env:UniversalCRTSdkDir) {
    Join-Path $env:UniversalCRTSdkDir "Redist\ucrt\DLLs\$Architecture"
  }
  if (-not $UcrtRedist -or -not (Test-Path $UcrtRedist)) {
    throw "找不到 Universal CRT 可再发行运行库目录；请安装 Windows SDK"
  }
  Copy-Item (Join-Path $UcrtRedist "*.dll") $Stage -Force

  # Qt 5.15.2 uses the OpenSSL 1.1.1 runtime backend on Windows. windeployqt
  # does not deploy these third-party DLLs, so a portable Win7 package must.
  if (-not $OpenSslRoot -or -not (Test-Path $OpenSslRoot)) {
    throw "Win7 package requires an OpenSSL runtime directory"
  }
  $OpenSslSuffix = if ($Architecture -eq "x64") { "-x64" } else { "" }
  foreach ($RuntimeName in @("libcrypto-1_1$OpenSslSuffix.dll", "libssl-1_1$OpenSslSuffix.dll")) {
    $RuntimePath = Join-Path $OpenSslRoot $RuntimeName
    if (-not (Test-Path $RuntimePath)) { throw "Missing Win7 TLS runtime: $RuntimePath" }
    Copy-Item $RuntimePath $Stage -Force
  }
  $OpenSslLicense = Join-Path $OpenSslRoot "LICENSE.txt"
  if (-not (Test-Path $OpenSslLicense)) { throw "Missing OpenSSL license: $OpenSslLicense" }
  $LicenseDir = Join-Path $Stage "licenses"
  New-Item -ItemType Directory -Force -Path $LicenseDir | Out-Null
  Copy-Item $OpenSslLicense (Join-Path $LicenseDir "OpenSSL-1.1.1w.txt") -Force

}

# Run QSslSocket from the final deployment directory for every Windows flavor.
# This catches a missing Qt 6 Schannel plugin as well as wrong/missing Qt 5
# OpenSSL DLLs before the ZIP is created. Do not ship the test executable.
$TlsProbeSource = Join-Path $Build "tests/windows_tls_runtime_probe.exe"
if (-not (Test-Path $TlsProbeSource)) { throw "Missing Windows TLS runtime probe" }
$TlsProbe = Join-Path $Stage "windows_tls_runtime_probe.exe"
Copy-Item $TlsProbeSource $TlsProbe -Force
try {
  & $TlsProbe
  if ($LASTEXITCODE -ne 0) { throw "Packaged Windows TLS runtime probe failed with exit code $LASTEXITCODE" }
} finally {
  Remove-Item $TlsProbe -Force
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
  if ($OcrPrefix) {
    Copy-Item (Join-Path $OcrPrefix "licenses") $Stage -Recurse -Force
  }

  # Exercise QtPdf, Tesseract, Leptonica, the qwindows platform plugin and the
  # bundled models from the final deployment directory, not the build tree.
  $OcrSmokeSource = Join-Path $Build "tests/document_extractor_test.exe"
  if (-not (Test-Path $OcrSmokeSource)) { throw "Missing packaged OCR runtime smoke test" }
  $OcrSmoke = Join-Path $Stage "document_extractor_test.exe"
  Copy-Item $OcrSmokeSource $OcrSmoke -Force
  $PreviousTessdataDir = $env:TESSDATA_DIR
  $PreviousTessdataPrefix = $env:TESSDATA_PREFIX
  $PreviousQtPluginPath = $env:QT_PLUGIN_PATH
  try {
    $env:TESSDATA_DIR = $null
    $env:TESSDATA_PREFIX = $null
    $env:QT_PLUGIN_PATH = $null
    & $OcrSmoke --ocr-smoke (Join-Path $Root "tests/fixtures/ocr-scan-fixture.pdf")
    if ($LASTEXITCODE -ne 0) { throw "Packaged Windows OCR runtime smoke failed with exit code $LASTEXITCODE" }
  } finally {
    $env:TESSDATA_DIR = $PreviousTessdataDir
    $env:TESSDATA_PREFIX = $PreviousTessdataPrefix
    $env:QT_PLUGIN_PATH = $PreviousQtPluginPath
    Remove-Item $OcrSmoke -Force
  }
}
$PlatformName = if ($Windows7Compat) { "windows7-$Architecture" } else { "windows-$Architecture" }
$PortableArchive = Join-Path $Dist "QuizPane-$PlatformName-portable$PackageSuffix.zip"
if (Test-Path $PortableArchive) { Remove-Item $PortableArchive -Force }

# 绿色版必须保留完整部署目录：Qt DLL、插件、OCR 语言数据与两个程序缺一不可。
# 直接压缩目录本身，使用户解压后得到一个独立文件夹，而不是散落到下载目录。
Compress-Archive -Path $Stage -DestinationPath $PortableArchive -CompressionLevel Optimal
Write-Host "已生成绿色版：$PortableArchive"
