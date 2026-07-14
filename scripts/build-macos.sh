#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build/release-macos}"
DIST_DIR="${DIST_DIR:-$ROOT/dist/macos}"
QT_PREFIX="${QT_PREFIX:-$(brew --prefix qt)}"
QT5COMPAT_PREFIX="${QT5COMPAT_PREFIX:-}"
TESSDATA_DIR="${TESSDATA_DIR:-$(brew --prefix)/share/tessdata}"
SIGN_IDENTITY="${SIGN_IDENTITY:--}"
CLEAN_BUILD="${CLEAN_BUILD:-1}"
DEBUG_BUILD="${DEBUG_BUILD:-0}"
VERBOSE_LOGS="${VERBOSE_LOGS:-0}"
export PKG_CONFIG_PATH="$(brew --prefix tesseract)/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# Homebrew 将 Qt5Compat 拆成独立 formula；Qt online installer 则把它放在 Qt
# 根目录。两种开发环境都传给 CMake，但 macdeployqt 仍使用主 Qt 的 bin 目录。
if [[ -z "$QT5COMPAT_PREFIX" ]]; then
  if [[ -d "$QT_PREFIX/lib/cmake/Qt6Core5Compat" ]]; then
    QT5COMPAT_PREFIX="$QT_PREFIX"
  elif command -v brew >/dev/null 2>&1 && brew list --versions qt5compat >/dev/null 2>&1; then
    QT5COMPAT_PREFIX="$(brew --prefix qt5compat)"
  fi
fi
CMAKE_PREFIX_PATH="$QT_PREFIX"
if [[ -n "$QT5COMPAT_PREFIX" ]]; then
  CMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH;$QT5COMPAT_PREFIX"
fi

BUILD_TYPE="Release"
DIAGNOSTIC_LOGGING="OFF"
PACKAGE_SUFFIX=""
VERBOSE_DIAGNOSTICS="OFF"
if [[ "$DEBUG_BUILD" == "1" ]]; then
  BUILD_TYPE="RelWithDebInfo"
  DIAGNOSTIC_LOGGING="ON"
  PACKAGE_SUFFIX="-debug"
fi
if [[ "$VERBOSE_LOGS" == "1" ]]; then
  if [[ "$DEBUG_BUILD" != "1" ]]; then
    echo "VERBOSE_LOGS=1 只能用于 DEBUG_BUILD=1" >&2
    exit 1
  fi
  VERBOSE_DIAGNOSTICS="ON"
fi

if [[ "$CLEAN_BUILD" == "1" ]]; then
  rm -rf "$BUILD_DIR"
fi

cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DCMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH" \
  -DQUIZPANE_ENABLE_TESSERACT_OCR=ON \
  -DQUIZPANE_PORTABLE_CPU_BASELINE=ON \
  -DQUIZPANE_ENABLE_DIAGNOSTIC_LOGGING="$DIAGNOSTIC_LOGGING" \
  -DQUIZPANE_ENABLE_VERBOSE_DIAGNOSTICS="$VERBOSE_DIAGNOSTICS" \
  -DQUIZPANE_BUILD_TESTS=ON
cmake --build "$BUILD_DIR" --parallel
ctest --test-dir "$BUILD_DIR" --output-on-failure

SOURCE_APP="$BUILD_DIR/apps/desktop-qt/小窗刷题.app"
SOURCE_STUDIO="$BUILD_DIR/apps/bank-studio/题库制作器.app"
mkdir -p "$DIST_DIR"
# 不让 macdeployqt 修改 CMake 构建产物。它对中文路径和二次部署的处理不稳定，
# 所以先复制到 ASCII staging，部署结束后再恢复中文 App 目录名。
STAGE_ROOT="$BUILD_DIR/package-stage"
rm -rf "$STAGE_ROOT"
mkdir -p "$STAGE_ROOT"
STAGED_APP="$STAGE_ROOT/QuizPane.app"
ditto "$SOURCE_APP" "$STAGED_APP"
deploy_app() {
  local app_path="$1"
  # macOS 自带的 Bash 3.2 在 set -u 下展开空数组会报 unbound variable，
  # 因此这里显式区分是否保留调试符号，不拼接可选参数数组。
  if [[ "$DEBUG_BUILD" == "1" ]]; then
    "$QT_PREFIX/bin/macdeployqt" "$app_path" -always-overwrite -no-strip
  else
    "$QT_PREFIX/bin/macdeployqt" "$app_path" -always-overwrite
  fi
}
deploy_app "$STAGED_APP"
STAGED_STUDIO="$STAGE_ROOT/QuizPaneQuestionMaker.app"
ditto "$SOURCE_STUDIO" "$STAGED_STUDIO"
if [[ ! -f "$TESSDATA_DIR/chi_sim.traineddata" || ! -f "$TESSDATA_DIR/eng.traineddata" ]]; then
  echo "缺少 OCR 语言数据：$TESSDATA_DIR/{chi_sim,eng}.traineddata" >&2
  exit 1
fi
mkdir -p "$STAGED_STUDIO/Contents/Resources/tessdata"
cp "$TESSDATA_DIR/chi_sim.traineddata" "$TESSDATA_DIR/eng.traineddata" \
  "$STAGED_STUDIO/Contents/Resources/tessdata/"
# 先递归收集 Tesseract/Leptonica 等 Homebrew 动态库。macdeployqt 会把非 Qt
# 依赖改写成 App 内路径却不复制源库，因此必须在它处理制作器之前完成收集。
dylibbundler -od -b \
  -x "$STAGED_STUDIO/Contents/MacOS/题库制作器" \
  -d "$STAGED_STUDIO/Contents/Frameworks" \
  -p @executable_path/../Frameworks
deploy_app "$STAGED_STUDIO"
if ! find "$STAGED_STUDIO/Contents/Frameworks" -iname '*tesseract*.dylib' -print -quit | grep -q .; then
  echo "OCR 打包失败：应用包中没有 Tesseract 动态库" >&2
  exit 1
fi
# dylibbundler 在 macdeployqt 之后加入的第三方库尚未剥离符号。签名前统一处理，
# 减少安装后体积，也让 ZIP 更容易压缩。
if [[ "$DEBUG_BUILD" != "1" ]]; then
  while IFS= read -r -d '' binary; do
    if file "$binary" | grep -q 'Mach-O'; then
      strip -x "$binary"
    fi
  done < <(find "$STAGED_STUDIO" -type f -print0)
fi
APP="$STAGE_ROOT/小窗刷题.app"
mv "$STAGED_APP" "$APP"
HELPERS="$APP/Contents/Helpers"
mkdir -p "$HELPERS"
STUDIO="$HELPERS/题库制作器.app"
mv "$STAGED_STUDIO" "$STUDIO"
# 制作器是主程序的一部分：先签 Helper，再签外层 App，确保 macOS 校验嵌套
# Bundle 时能够追溯到同一份发行包。
codesign --force --deep --options runtime --sign "$SIGN_IDENTITY" "$STUDIO"
codesign --force --deep --options runtime --sign "$SIGN_IDENTITY" "$APP"
codesign --verify --deep --strict "$APP"

DMG_STAGE="$BUILD_DIR/dmg-stage"
rm -rf "$DMG_STAGE"
mkdir -p "$DMG_STAGE"
ditto "$APP" "$DMG_STAGE/小窗刷题.app"
ln -s /Applications "$DMG_STAGE/Applications"
DMG="$DIST_DIR/QuizPane-macos-$(uname -m)$PACKAGE_SUFFIX.dmg"
rm -f "$DMG"
hdiutil create -volname "QuizPane" -srcfolder "$DMG_STAGE" -ov -format UDZO "$DMG"

echo "已生成：$DMG"
du -h "$DMG"
