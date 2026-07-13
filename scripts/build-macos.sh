#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build/release-macos}"
DIST_DIR="${DIST_DIR:-$ROOT/dist/macos}"
QT_PREFIX="${QT_PREFIX:-$(brew --prefix qtbase)}"
SIGN_IDENTITY="${SIGN_IDENTITY:--}"
CLEAN_BUILD="${CLEAN_BUILD:-1}"

if [[ "$CLEAN_BUILD" == "1" ]]; then
  rm -rf "$BUILD_DIR"
fi

cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
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
DEPLOY_ARGS=("$STAGED_APP" -always-overwrite)
"$QT_PREFIX/bin/macdeployqt" "${DEPLOY_ARGS[@]}"
STAGED_STUDIO="$STAGE_ROOT/QuizPaneQuestionMaker.app"
ditto "$SOURCE_STUDIO" "$STAGED_STUDIO"
"$QT_PREFIX/bin/macdeployqt" "$STAGED_STUDIO" -always-overwrite
APP="$STAGE_ROOT/小窗刷题.app"
mv "$STAGED_APP" "$APP"
STUDIO="$STAGE_ROOT/题库制作器.app"
mv "$STAGED_STUDIO" "$STUDIO"
codesign --force --deep --options runtime --sign "$SIGN_IDENTITY" "$APP"
codesign --force --deep --options runtime --sign "$SIGN_IDENTITY" "$STUDIO"
codesign --verify --deep --strict "$APP"
codesign --verify --deep --strict "$STUDIO"
ditto -c -k --sequesterRsrc --keepParent "$APP" \
  "$DIST_DIR/小窗刷题-macos-$(uname -m).zip"
ditto -c -k --sequesterRsrc --keepParent "$STUDIO" \
  "$DIST_DIR/题库制作器-macos-$(uname -m).zip"

echo "已生成：$DIST_DIR/小窗刷题-macos-$(uname -m).zip"
echo "已生成：$DIST_DIR/题库制作器-macos-$(uname -m).zip"
