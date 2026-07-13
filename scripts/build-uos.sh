#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build/release-uos}"
DIST_DIR="${DIST_DIR:-$ROOT/dist/uos}"
QT_PREFIX="${QT_PREFIX:-}"
# Linux 代码和构建流程在 UOS、银河麒麟之间共用。发行版标签只影响产物名称，
# 例如 DISTRO_ID=kylin 会生成“小窗刷题-kylin-x86_64.tar.gz”。
DISTRO_ID="${DISTRO_ID:-uos}"
ARCH="$(uname -m)"
PREFIX_ARGS=()
if [[ -n "$QT_PREFIX" ]]; then PREFIX_ARGS=(-DCMAKE_PREFIX_PATH="$QT_PREFIX"); fi

cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DQUIZPANE_BUILD_TESTS=ON "${PREFIX_ARGS[@]}"
cmake --build "$BUILD_DIR" --parallel
ctest --test-dir "$BUILD_DIR" --output-on-failure

APPDIR="$DIST_DIR/小窗刷题.AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/applications" \
  "$APPDIR/usr/share/icons/hicolor/1024x1024/apps"
cp "$BUILD_DIR/apps/desktop-qt/小窗刷题" "$APPDIR/usr/bin/"
cp "$ROOT/apps/desktop-qt/resources/app-icon.png" \
  "$APPDIR/usr/share/icons/hicolor/1024x1024/apps/org.quizpane.app.png"
cp "$ROOT/packaging/linux/org.quizpane.app.desktop" \
  "$APPDIR/usr/share/applications/"
cp "$ROOT/LICENSE" "$APPDIR/"
mkdir -p "$DIST_DIR"
tar -C "$DIST_DIR" -czf "$DIST_DIR/小窗刷题-$DISTRO_ID-$ARCH.tar.gz" \
  "小窗刷题.AppDir"
if command -v linuxdeploy >/dev/null 2>&1; then
  OUTPUT="$DIST_DIR/小窗刷题-$DISTRO_ID-$ARCH.AppImage" \
    linuxdeploy --appdir "$APPDIR" --output appimage
fi
echo "已生成：$DIST_DIR/小窗刷题-$DISTRO_ID-$ARCH.tar.gz"
