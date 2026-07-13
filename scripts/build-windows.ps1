param(
    [string]$QtRoot = $env:QT_ROOT,
    [string]$BuildDir = "build/release-windows",
    [string]$DistDir = "dist/windows"
)
$ErrorActionPreference = "Stop"
if (-not $QtRoot) { throw "请通过 -QtRoot 或 QT_ROOT 指定 Qt 6 的 MSVC 目录" }

$Root = (Resolve-Path "$PSScriptRoot/..").Path
$Build = Join-Path $Root $BuildDir
$Dist = Join-Path $Root $DistDir
cmake -S $Root -B $Build -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_PREFIX_PATH=$QtRoot `
  -DQUIZPANE_BUILD_TESTS=ON
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
  Copy-Item (Join-Path $Root "LICENSE") $Stage -Force
  & (Join-Path $QtRoot "bin/windeployqt.exe") --release --no-translations `
    (Join-Path $Stage $Package.Executable)

  $Zip = Join-Path $Dist "$($Package.Name)-windows-x64.zip"
  if (Test-Path $Zip) { Remove-Item $Zip -Force }
  Compress-Archive -Path "$Stage/*" -DestinationPath $Zip
  Write-Host "已生成：$Zip"
}
