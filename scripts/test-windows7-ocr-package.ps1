param(
    [Parameter(Mandatory = $true)][string]$PackageRoot,
    [Parameter(Mandatory = $true)][string]$BuildDir
)
$ErrorActionPreference = "Stop"
$RepositoryRoot = (Resolve-Path "$PSScriptRoot/..").Path
$PackageRoot = (Resolve-Path $PackageRoot).Path
foreach ($File in @("tessdata/chi_sim.traineddata", "tessdata/eng.traineddata",
                    "licenses/tesseract.txt", "licenses/leptonica.txt", "licenses/tessdata-fast.txt")) {
    if (-not (Test-Path (Join-Path $PackageRoot $File))) { throw "OCR ZIP 缺少 $File" }
}
# A tripwire for common Win8+ imports, NOT a complete Win7 API certification.
$Unsupported = '\b(WaitOnAddress|WakeByAddressSingle|WakeByAddressAll|GetSystemTimePreciseAsFileTime|GetCurrentThreadStackLimits|SetThreadDescription|GetTempPath2[AW]|CreateFile2|GetDpiForWindow|GetDpiForSystem|SetProcessDpiAwarenessContext)\b'
foreach ($Binary in (Get-ChildItem $PackageRoot -File | Where-Object { $_.Extension -in @(".exe", ".dll") })) {
    $Imports = & dumpbin /imports $Binary.FullName
    if ($LASTEXITCODE -ne 0) { throw "无法检查 $($Binary.Name) 的导入表" }
    if ($Imports -match $Unsupported) { throw "$($Binary.Name) 引用了已知的 Win7 不支持的 API" }
}
# Tests execute against the extracted ZIP's Qt runtimes and models, not PATH,
# vcpkg, system tessdata or the original build directory. Do not ship this EXE.
$Smoke = Join-Path $PackageRoot "document_extractor_test.exe"
if (Test-Path $Smoke) { throw "冒烟测试目标已存在，拒绝覆盖：$Smoke" }
Copy-Item (Join-Path $BuildDir "tests/document_extractor_test.exe") $Smoke -Force
$OldData = $env:TESSDATA_DIR
$OldPrefix = $env:TESSDATA_PREFIX
$OldPlugins = $env:QT_PLUGIN_PATH
$OldPlatformPlugins = $env:QT_QPA_PLATFORM_PLUGIN_PATH
try {
    $env:TESSDATA_DIR = $null
    $env:TESSDATA_PREFIX = $null
    $env:QT_PLUGIN_PATH = $null
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = $null
    & $Smoke --ocr-smoke "$RepositoryRoot/tests/fixtures/ocr-scan-fixture.pdf"
    if ($LASTEXITCODE -ne 0) { throw "解压包 OCR 冒烟失败" }
    # A damaged Chinese model must not silently fall back to English-only OCR.
    $Chinese = Join-Path $PackageRoot "tessdata/chi_sim.traineddata"
    $Backup = "$Chinese.smoke-backup"
    if (Test-Path $Backup) { throw "存在未恢复的模型备份：$Backup" }
    Move-Item $Chinese $Backup
    try {
        [System.IO.File]::WriteAllText($Chinese, "intentional invalid model for smoke test")
        $Failure = & $Smoke --ocr-smoke "$RepositoryRoot/tests/fixtures/ocr-scan-fixture.pdf" 2>&1
        if ($LASTEXITCODE -ne 8 -or -not ($Failure -match "识别模型")) {
            throw "模型损坏时 OCR 未给出预期错误（退出码 $LASTEXITCODE）：$Failure"
        }
    } finally {
        Move-Item $Backup $Chinese -Force
    }
} finally {
    $env:TESSDATA_DIR = $OldData
    $env:TESSDATA_PREFIX = $OldPrefix
    $env:QT_PLUGIN_PATH = $OldPlugins
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = $OldPlatformPlugins
    Remove-Item $Smoke
}
# The negative test intentionally exits nonzero; do not leak it to Actions.
$global:LASTEXITCODE = 0
Write-Host "OCR ZIP smoke passed (hosted runner only; Win7 SP1 VM acceptance is still required)."
