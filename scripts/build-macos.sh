#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build/release-macos}"
DIST_DIR="${DIST_DIR:-$ROOT/dist/macos}"
QT_PREFIX="${QT_PREFIX:-$(brew --prefix qt)}"
TESSDATA_DIR="${TESSDATA_DIR:-$(brew --prefix)/share/tessdata}"
SIGN_IDENTITY="${SIGN_IDENTITY:--}"
CLEAN_BUILD="${CLEAN_BUILD:-1}"
DEBUG_BUILD="${DEBUG_BUILD:-0}"
VERBOSE_LOGS="${VERBOSE_LOGS:-0}"
export PKG_CONFIG_PATH="$(brew --prefix tesseract)/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

BUILD_TYPE="Release"
DIAGNOSTIC_LOGGING="OFF"
PACKAGE_SUFFIX=""
DEPLOY_DEBUG_ARGS=()
VERBOSE_DIAGNOSTICS="OFF"
if [[ "$DEBUG_BUILD" == "1" ]]; then
  BUILD_TYPE="RelWithDebInfo"
  DIAGNOSTIC_LOGGING="ON"
  PACKAGE_SUFFIX="-debug"
  DEPLOY_DEBUG_ARGS=(-no-strip)
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
  -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
  -DQUIZPANE_ENABLE_TESSERACT_OCR=ON \
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
DEPLOY_ARGS=("$STAGED_APP" -always-overwrite "${DEPLOY_DEBUG_ARGS[@]}")
"$QT_PREFIX/bin/macdeployqt" "${DEPLOY_ARGS[@]}"
STAGED_STUDIO="$STAGE_ROOT/QuizPaneQuestionMaker.app"
ditto "$SOURCE_STUDIO" "$STAGED_STUDIO"
"$QT_PREFIX/bin/macdeployqt" "$STAGED_STUDIO" -always-overwrite "${DEPLOY_DEBUG_ARGS[@]}"
if [[ ! -f "$TESSDATA_DIR/chi_sim.traineddata" || ! -f "$TESSDATA_DIR/eng.traineddata" ]]; then
  echo "缺少 OCR 语言数据：$TESSDATA_DIR/{chi_sim,eng}.traineddata" >&2
  exit 1
fi
mkdir -p "$STAGED_STUDIO/Contents/Resources/tessdata"
cp "$TESSDATA_DIR/chi_sim.traineddata" "$TESSDATA_DIR/eng.traineddata" \
  "$STAGED_STUDIO/Contents/Resources/tessdata/"
# macdeployqt 只负责 Qt；继续递归收集 Tesseract/Leptonica 等 Homebrew 动态库。
dylibbundler -od -b \
  -x "$STAGED_STUDIO/Contents/MacOS/题库制作器" \
  -d "$STAGED_STUDIO/Contents/Frameworks" \
  -p @executable_path/../Frameworks
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
STUDIO="$STAGE_ROOT/题库制作器.app"
mv "$STAGED_STUDIO" "$STUDIO"
codesign --force --deep --options runtime --sign "$SIGN_IDENTITY" "$APP"
codesign --force --deep --options runtime --sign "$SIGN_IDENTITY" "$STUDIO"
codesign --verify --deep --strict "$APP"
codesign --verify --deep --strict "$STUDIO"
ditto -c -k --zlibCompressionLevel 9 --sequesterRsrc --keepParent "$APP" \
  "$DIST_DIR/小窗刷题-macos-$(uname -m)$PACKAGE_SUFFIX.zip"
ditto -c -k --zlibCompressionLevel 9 --sequesterRsrc --keepParent "$STUDIO" \
  "$DIST_DIR/题库制作器-macos-$(uname -m)$PACKAGE_SUFFIX.zip"

echo "已生成：$DIST_DIR/小窗刷题-macos-$(uname -m)$PACKAGE_SUFFIX.zip"
echo "已生成：$DIST_DIR/题库制作器-macos-$(uname -m)$PACKAGE_SUFFIX.zip"
du -h "$DIST_DIR/小窗刷题-macos-$(uname -m)$PACKAGE_SUFFIX.zip" \
  "$DIST_DIR/题库制作器-macos-$(uname -m)$PACKAGE_SUFFIX.zip"
