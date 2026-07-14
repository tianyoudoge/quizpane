#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build/release-uos}"
DIST_DIR="${DIST_DIR:-$ROOT/dist/uos}"
QT_PREFIX="${QT_PREFIX:-}"
TESSDATA_DIR="${TESSDATA_DIR:-}"
DEBUG_BUILD="${DEBUG_BUILD:-0}"
VERBOSE_LOGS="${VERBOSE_LOGS:-0}"
# Linux 代码和构建流程在 UOS、银河麒麟之间共用。发行版标签只影响产物名称，
# 例如 DISTRO_ID=kylin 会生成“小窗刷题-kylin-x86_64.tar.gz”。
DISTRO_ID="${DISTRO_ID:-uos}"
ARCH="$(uname -m)"
PREFIX_ARGS=()
if [[ -n "$QT_PREFIX" ]]; then PREFIX_ARGS=(-DCMAKE_PREFIX_PATH="$QT_PREFIX"); fi
BUILD_TYPE="Release"
DIAGNOSTIC_LOGGING="OFF"
PACKAGE_SUFFIX=""
VERBOSE_DIAGNOSTICS="OFF"

# 不同发行版会把语言数据安装到 4.00、4 或 5 等版本目录。优先尊重调用方
# 指定的路径，其次读取 Tesseract 的 pkg-config 元数据，最后检查常见目录。
if [[ -z "$TESSDATA_DIR" ]] && command -v pkg-config >/dev/null 2>&1; then
  TESSDATA_DIR="$(pkg-config --variable=tessdata_dir tesseract 2>/dev/null || true)"
fi
if [[ ! -f "$TESSDATA_DIR/chi_sim.traineddata" || ! -f "$TESSDATA_DIR/eng.traineddata" ]]; then
  for candidate in \
    /usr/share/tesseract-ocr/5/tessdata \
    /usr/share/tesseract-ocr/4.00/tessdata \
    /usr/share/tesseract-ocr/4/tessdata \
    /usr/share/tessdata; do
    if [[ -f "$candidate/chi_sim.traineddata" && -f "$candidate/eng.traineddata" ]]; then
      TESSDATA_DIR="$candidate"
      break
    fi
  done
fi

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

cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DQUIZPANE_ENABLE_TESSERACT_OCR=ON \
  -DQUIZPANE_PORTABLE_CPU_BASELINE=ON \
  -DQUIZPANE_ENABLE_DIAGNOSTIC_LOGGING="$DIAGNOSTIC_LOGGING" \
  -DQUIZPANE_ENABLE_VERBOSE_DIAGNOSTICS="$VERBOSE_DIAGNOSTICS" \
  -DQUIZPANE_BUILD_TESTS=ON "${PREFIX_ARGS[@]}"
cmake --build "$BUILD_DIR" --parallel
ctest --test-dir "$BUILD_DIR" --output-on-failure

mkdir -p "$DIST_DIR"

create_archives() {
  local package_name="$1"
  local appdir_name="$2"
  local stem="$DIST_DIR/$package_name-$DISTRO_ID-$ARCH$PACKAGE_SUFFIX"
  tar -C "$DIST_DIR" -cf - "$appdir_name" | gzip -9 -n > "$stem.tar.gz"
  # xz 明显偏向最小下载体积；保留 tar.gz 给缺少 xz 的老系统兜底。
  if command -v xz >/dev/null 2>&1; then
    tar -C "$DIST_DIR" -cf - "$appdir_name" | xz -9e -T1 > "$stem.tar.xz"
  fi
  du -h "$stem.tar.gz" "$stem.tar.xz" 2>/dev/null || true
}

strip_elf_files() {
  local appdir="$1"
  while IFS= read -r -d '' binary; do
    if file "$binary" | grep -q 'ELF'; then
      strip --strip-unneeded "$binary"
    fi
  done < <(find "$appdir/usr/bin" "$appdir/usr/lib" -type f -print0 2>/dev/null)
}

package_app() {
  local package_name="$1"
  local executable="$2"
  local source_binary="$3"
  local desktop_file="$4"
  local icon_source="$5"
  local icon_id="$6"
  local appdir="$DIST_DIR/$package_name.AppDir"

  rm -rf "$appdir"
  mkdir -p "$appdir/usr/bin" "$appdir/usr/share/applications" \
    "$appdir/usr/share/icons/hicolor/512x512/apps"
  cp "$source_binary" "$appdir/usr/bin/$executable"
  local deployed_icon="$appdir/usr/share/icons/hicolor/512x512/apps/$icon_id.png"
  if command -v magick >/dev/null 2>&1; then
    magick "$icon_source" -resize 512x512 "$deployed_icon"
  elif command -v convert >/dev/null 2>&1; then
    convert "$icon_source" -resize 512x512 "$deployed_icon"
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
  if [[ "$DEBUG_BUILD" != "1" ]]; then
    strip_elf_files "$appdir"
  fi
}

package_app "QuizPane" "小窗刷题" \
  "$BUILD_DIR/apps/desktop-qt/小窗刷题" \
  "$ROOT/packaging/linux/org.quizpane.app.desktop" \
  "$ROOT/apps/desktop-qt/resources/app-icon.png" \
  "org.quizpane.app"
mkdir -p "$DIST_DIR/QuizPane.AppDir/usr/share/mime/packages"
cp "$ROOT/packaging/linux/org.quizpane.provider.xml" \
  "$DIST_DIR/QuizPane.AppDir/usr/share/mime/packages/"
create_archives "QuizPane" "QuizPane.AppDir"

package_app "QuizPane-Question-Maker" "题库制作器" \
  "$BUILD_DIR/apps/bank-studio/题库制作器" \
  "$ROOT/packaging/linux/org.quizpane.bank-studio.desktop" \
  "$ROOT/apps/bank-studio/resources/app-icon.png" \
  "org.quizpane.question-maker"

if ! find "$DIST_DIR/QuizPane-Question-Maker.AppDir" -name 'libtesseract.so*' -print -quit | grep -q .; then
  echo "OCR 打包失败：制作器目录包中没有 Tesseract 运行库（请安装并使用 linuxdeploy）" >&2
  exit 1
fi
STUDIO_TESSDATA="$DIST_DIR/QuizPane-Question-Maker.AppDir/usr/share/quizpane/tessdata"
if [[ ! -f "$TESSDATA_DIR/chi_sim.traineddata" || ! -f "$TESSDATA_DIR/eng.traineddata" ]]; then
  echo "缺少 OCR 语言数据：$TESSDATA_DIR/{chi_sim,eng}.traineddata" >&2
  exit 1
fi
mkdir -p "$STUDIO_TESSDATA"
cp "$TESSDATA_DIR/chi_sim.traineddata" "$TESSDATA_DIR/eng.traineddata" "$STUDIO_TESSDATA/"
create_archives "QuizPane-Question-Maker" "QuizPane-Question-Maker.AppDir"
