param(
    [Parameter(Mandatory = $true)][string]$PackageRoot,
    [Parameter(Mandatory = $true)][string]$BuildDir
)
$ErrorActionPreference = "Stop"
$RepositoryRoot = (Resolve-Path "$PSScriptRoot/..").Path
$PackageRoot = (Resolve-Path $PackageRoot).Path

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$ArgumentList,
        [int]$TimeoutSeconds = 60
    )
    $StartInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $StartInfo.FileName = $FilePath
    $StartInfo.UseShellExecute = $false
    $StartInfo.CreateNoWindow = $true
    $StartInfo.RedirectStandardOutput = $true
    $StartInfo.RedirectStandardError = $true
    foreach ($Argument in $ArgumentList) { $StartInfo.ArgumentList.Add($Argument) }
    $Process = [System.Diagnostics.Process]::new()
    $Process.StartInfo = $StartInfo
    try {
        if (-not $Process.Start()) { throw "无法启动 $FilePath" }
        $StdoutTask = $Process.StandardOutput.ReadToEndAsync()
        $StderrTask = $Process.StandardError.ReadToEndAsync()
        $TimedOut = -not $Process.WaitForExit($TimeoutSeconds * 1000)
        if ($TimedOut) {
            try { $Process.Kill($true) } catch { $Process.Kill() }
            $Process.WaitForExit()
        }
        $Stdout = $StdoutTask.GetAwaiter().GetResult()
        $Stderr = $StderrTask.GetAwaiter().GetResult()
        if ($TimedOut) { throw "$FilePath 超过 $TimeoutSeconds 秒未退出" }
        return [pscustomobject]@{
            ExitCode = $Process.ExitCode
            Stdout = $Stdout
            Stderr = $Stderr
        }
    } finally {
        $Process.Dispose()
    }
}

# Loader failures normally display a modal dialog on Windows. CI has no user to
# dismiss it, so child smoke processes must inherit non-interactive error mode.
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class QuizPaneNativeMethods {
    [DllImport("kernel32.dll")]
    public static extern uint SetErrorMode(uint mode);
}
"@
$PreviousErrorMode = [QuizPaneNativeMethods]::SetErrorMode(0x8003)

foreach ($File in @("tessdata/chi_sim.traineddata", "tessdata/eng.traineddata",
                    "licenses/tesseract.txt", "licenses/leptonica.txt", "licenses/tessdata-fast.txt")) {
    if (-not (Test-Path (Join-Path $PackageRoot $File))) { throw "OCR ZIP 缺少 $File" }
}
# A tripwire for common Win8+ imports, NOT a complete Win7 API certification.
$Unsupported = '\b(WaitOnAddress|WakeByAddressSingle|WakeByAddressAll|GetSystemTimePreciseAsFileTime|GetCurrentThreadStackLimits|SetThreadDescription|GetTempPath2[AW]|CreateFile2|GetDpiForWindow|GetDpiForSystem|SetProcessDpiAwarenessContext)\b'
$Dumpbin = (Get-Command dumpbin.exe -ErrorAction Stop).Source
foreach ($Binary in (Get-ChildItem $PackageRoot -File | Where-Object { $_.Extension -in @(".exe", ".dll") })) {
    Write-Host "Checking Win7 imports: $($Binary.Name)"
    $DumpbinResult = Invoke-NativeCommand -FilePath $Dumpbin `
        -ArgumentList @("/imports", $Binary.FullName) -TimeoutSeconds 30
    if ($DumpbinResult.ExitCode -ne 0) {
        throw "无法检查 $($Binary.Name) 的导入表：$($DumpbinResult.Stderr)"
    }
    $Imports = $DumpbinResult.Stdout
    if ($Imports -match $Unsupported) { throw "$($Binary.Name) 引用了已知的 Win7 不支持的 API" }
}
# Tests execute against the extracted ZIP's Qt runtimes and models, not PATH,
# vcpkg, system tessdata or the original build directory. Do not ship this EXE.
$Smoke = Join-Path $PackageRoot "document_extractor_test.exe"
if (Test-Path $Smoke) { throw "冒烟测试目标已存在，拒绝覆盖：$Smoke" }
Copy-Item (Join-Path $BuildDir "tests/document_extractor_test.exe") $Smoke -Force
$Dependencies = Invoke-NativeCommand -FilePath $Dumpbin `
    -ArgumentList @("/dependents", $Smoke) -TimeoutSeconds 30
if ($Dependencies.ExitCode -ne 0) { throw "无法检查 OCR 冒烟程序依赖" }
Write-Host "OCR smoke direct dependencies:"
Write-Host $Dependencies.Stdout
$OldData = $env:TESSDATA_DIR
$OldPrefix = $env:TESSDATA_PREFIX
$OldPlugins = $env:QT_PLUGIN_PATH
$OldPlatformPlugins = $env:QT_QPA_PLATFORM_PLUGIN_PATH
try {
    $env:TESSDATA_DIR = $null
    $env:TESSDATA_PREFIX = $null
    $env:QT_PLUGIN_PATH = $null
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = $null
    Write-Host "Running OCR smoke from extracted package"
    $SmokeResult = Invoke-NativeCommand -FilePath $Smoke `
        -ArgumentList @("--ocr-smoke", "$RepositoryRoot/tests/fixtures/ocr-scan-fixture.pdf") `
        -TimeoutSeconds 60
    $SmokeOutput = "$($SmokeResult.Stdout)`n$($SmokeResult.Stderr)"
    Write-Host $SmokeOutput
    if ($SmokeResult.ExitCode -ne 0) {
        throw "解压包 OCR 冒烟失败（退出码 $($SmokeResult.ExitCode)）：$SmokeOutput"
    }
    # A damaged Chinese model must not silently fall back to English-only OCR.
    $Chinese = Join-Path $PackageRoot "tessdata/chi_sim.traineddata"
    $Backup = "$Chinese.smoke-backup"
    if (Test-Path $Backup) { throw "存在未恢复的模型备份：$Backup" }
    Move-Item $Chinese $Backup
    try {
        [System.IO.File]::WriteAllText($Chinese, "intentional invalid model for smoke test")
        Write-Host "Running damaged-model OCR smoke"
        $FailureResult = Invoke-NativeCommand -FilePath $Smoke `
            -ArgumentList @("--ocr-smoke", "$RepositoryRoot/tests/fixtures/ocr-scan-fixture.pdf") `
            -TimeoutSeconds 60
        $Failure = "$($FailureResult.Stdout)`n$($FailureResult.Stderr)"
        Write-Host $Failure
        if ($FailureResult.ExitCode -ne 8 -or -not ($Failure -match "识别模型")) {
            throw "模型损坏时 OCR 未给出预期错误（退出码 $($FailureResult.ExitCode)）：$Failure"
        }
    } finally {
        Move-Item $Backup $Chinese -Force
    }
} finally {
    $env:TESSDATA_DIR = $OldData
    $env:TESSDATA_PREFIX = $OldPrefix
    $env:QT_PLUGIN_PATH = $OldPlugins
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = $OldPlatformPlugins
    [QuizPaneNativeMethods]::SetErrorMode($PreviousErrorMode) | Out-Null
    Remove-Item $Smoke
}
# The negative test intentionally exits nonzero; do not leak it to Actions.
$global:LASTEXITCODE = 0
Write-Host "OCR ZIP smoke passed (hosted runner only; Win7 SP1 VM acceptance is still required)."
