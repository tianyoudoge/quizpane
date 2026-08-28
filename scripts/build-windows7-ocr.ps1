param(
    [ValidateSet("x64", "x86")]
    [string]$Architecture = "x64",
    [string]$BuildDir = ""
)
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path "$PSScriptRoot/..").Path
if (-not $BuildDir) { $BuildDir = Join-Path $Root "build/windows7-ocr-$Architecture" }
if ($env:VCToolsVersion -notlike "14.29.*") {
    throw "Win7 OCR 必须在 MSVC v142/14.29 开发者命令行中编译"
}
if ($env:VSCMD_ARG_TGT_ARCH -ne $Architecture) {
    throw "开发者命令行架构与 OCR 目标架构 $Architecture 不一致"
}
cmake -S "$Root/cmake/win7-ocr" -B $BuildDir -G Ninja
if ($LASTEXITCODE -ne 0) { throw "Win7 OCR 依赖配置失败" }
cmake --build $BuildDir --parallel
if ($LASTEXITCODE -ne 0) { throw "Win7 OCR 依赖编译失败" }
cmake "-DOCR_INSTALL_DIR=$BuildDir/install" -P "$Root/cmake/win7-ocr/models.cmake"
if ($LASTEXITCODE -ne 0) { throw "Win7 OCR 语言模型下载或校验失败" }
Write-Host "Win7 OCR candidate dependencies: $BuildDir/install"
