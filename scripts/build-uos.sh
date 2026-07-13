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

mkdir -p "$DIST_DIR"

package_app() {
  local package_name="$1"
  local executable="$2"
  local source_binary="$3"
  local desktop_file="$4"
  local appdir="$DIST_DIR/$package_name.AppDir"
  local archive="$DIST_DIR/$package_name-$DISTRO_ID-$ARCH.tar.gz"

  rm -rf "$appdir"
  mkdir -p "$appdir/usr/bin" "$appdir/usr/share/applications" \
    "$appdir/usr/share/icons/hicolor/512x512/apps"
  cp "$source_binary" "$appdir/usr/bin/$executable"
  local deployed_icon="$appdir/usr/share/icons/hicolor/512x512/apps/org.quizpane.app.png"
  if command -v magick >/dev/null 2>&1; then
    magick "$ROOT/apps/desktop-qt/resources/app-icon.png" -resize 512x512 "$deployed_icon"
  elif command -v convert >/dev/null 2>&1; then
    convert "$ROOT/apps/desktop-qt/resources/app-icon.png" -resize 512x512 "$deployed_icon"
  else
    echo "缺少 ImageMagick，无法生成符合 Linux 桌面规范的 512x512 图标" >&2
    return 1
  fi
  cp "$desktop_file" "$appdir/usr/share/applications/"
  cp "$ROOT/LICENSE" "$appdir/"

  if command -v linuxdeploy >/dev/null 2>&1; then
    QMAKE="${QT_PREFIX:+$QT_PREFIX/bin/qmake}" \
      linuxdeploy --appdir "$appdir" \
        --desktop-file "$desktop_file" \
        --icon-file "$deployed_icon" \
        --executable "$appdir/usr/bin/$executable" \
        --plugin qt
  fi
  tar -C "$DIST_DIR" -czf "$archive" "$package_name.AppDir"
  echo "已生成：$archive"
}

package_app "QuizPane" "小窗刷题" \
  "$BUILD_DIR/apps/desktop-qt/小窗刷题" \
  "$ROOT/packaging/linux/org.quizpane.app.desktop"
mkdir -p "$DIST_DIR/QuizPane.AppDir/usr/share/mime/packages"
cp "$ROOT/packaging/linux/org.quizpane.provider.xml" \
  "$DIST_DIR/QuizPane.AppDir/usr/share/mime/packages/"
# MIME 文件加入后重新生成主程序压缩包。
tar -C "$DIST_DIR" -czf "$DIST_DIR/QuizPane-$DISTRO_ID-$ARCH.tar.gz" \
  "QuizPane.AppDir"

package_app "QuizPane-Bank-Studio" "题库生成器" \
  "$BUILD_DIR/apps/bank-studio/题库生成器" \
  "$ROOT/packaging/linux/org.quizpane.bank-studio.desktop"
