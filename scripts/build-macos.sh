#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build/release-macos}"
DIST_DIR="${DIST_DIR:-$ROOT/dist/macos}"
QT_PREFIX="${QT_PREFIX:-$(brew --prefix qt)}"
QT5COMPAT_PREFIX="${QT5COMPAT_PREFIX:-}"
TESSDATA_DIR="${TESSDATA_DIR:-$(brew --prefix tesseract)/share/tessdata}"
TESSDATA_LANG_DIR="${TESSDATA_LANG_DIR:-$(brew --prefix tesseract-lang)/share/tessdata}"
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

# Homebrew 的聚合 qt formula 可能不再直接提供 macdeployqt，而是由 qtbase
# formula 提供；优先尊重显式配置，再依次兼容两种 Homebrew 布局。
MACDEPLOYQT="${MACDEPLOYQT:-}"
if [[ -z "$MACDEPLOYQT" && -x "$QT_PREFIX/bin/macdeployqt" ]]; then
  MACDEPLOYQT="$QT_PREFIX/bin/macdeployqt"
elif [[ -z "$MACDEPLOYQT" ]] && command -v macdeployqt >/dev/null 2>&1; then
  MACDEPLOYQT="$(command -v macdeployqt)"
fi
if [[ -z "$MACDEPLOYQT" || ! -x "$MACDEPLOYQT" ]]; then
  echo "未找到 macdeployqt；请安装 qtbase 或设置 MACDEPLOYQT" >&2
  exit 1
fi

# Qt 的 Homebrew 拆包会把 QtSvg、QtPdf 等 Framework 放在各自 formula
# 的 lib 目录。macdeployqt 默认只搜索 qtbase，因而需要把已安装模块显式
# 加进查找路径，才能收集制作器与主程序的全部 Qt Framework。
QT_DEPLOY_LIB_PATHS=()
if [[ -d "$QT_PREFIX/lib" ]]; then
  QT_DEPLOY_LIB_PATHS+=("$QT_PREFIX/lib")
fi
for qt_module in qtbase qtsvg qtwebengine; do
  if brew list --versions "$qt_module" >/dev/null 2>&1; then
    qt_module_lib="$(brew --prefix "$qt_module")/lib"
    if [[ -d "$qt_module_lib" ]]; then
      QT_DEPLOY_LIB_PATHS+=("$qt_module_lib")
    fi
  fi
done
if [[ ${#QT_DEPLOY_LIB_PATHS[@]} -eq 0 ]]; then
  echo "未找到 Qt Framework 搜索目录" >&2
  exit 1
fi

# install-qt-action 把模块放在 QT_PREFIX/lib；Homebrew 拆包环境才需要到各
# formula 下查找。优先使用当前构建所用的 Qt，避免混入另一套 Qt 安装。
resolve_qt_framework() {
  local framework="$1"
  local formula="$2"
  local candidate="$QT_PREFIX/lib/$framework.framework"
  if [[ -d "$candidate" ]]; then
    printf '%s\n' "$candidate"
    return 0
  fi
  if command -v brew >/dev/null 2>&1 && brew list --versions "$formula" >/dev/null 2>&1; then
    candidate="$(brew --prefix "$formula")/lib/$framework.framework"
    if [[ -d "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  fi
  echo "缺少 Qt Framework：$framework（QT_PREFIX=$QT_PREFIX）" >&2
  return 1
}
QT_SVG_FRAMEWORK="$(resolve_qt_framework QtSvg qtsvg)"
QT_PDF_FRAMEWORK="$(resolve_qt_framework QtPdf qtwebengine)"

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

cmake --preset release -S "$ROOT" -B "$BUILD_DIR" \
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
  local deploy_args=("$MACDEPLOYQT" "$app_path" -always-overwrite)
  # Homebrew 第三方 dylib 可能带着已因 install_name 改写而失效的 ad-hoc 签名。
  # macdeployqt 默认会在部署中途验证它并提前失败；最终本来就会统一签整个 App，
  # 因此支持 -no-codesign 的 Qt 版本先关闭中途签名。旧版没有该参数时保持默认。
  local deploy_help
  deploy_help="$("$MACDEPLOYQT" -help 2>&1 || true)"
  if grep -q -- '-no-codesign' <<<"$deploy_help"; then
    deploy_args+=(-no-codesign)
  fi
  local lib_path
  for lib_path in "${QT_DEPLOY_LIB_PATHS[@]}"; do
    deploy_args+=("-libpath=$lib_path")
  done
  # QtPdf/QtSvg 已先复制进 App；把目标 Frameworks 目录也传给 macdeployqt，
  # 否则 Homebrew 拆包环境会在部署扫描 rpath 时误判这两个直接依赖缺失。
  deploy_args+=("-libpath=$app_path/Contents/Frameworks")
  # macOS 自带的 Bash 3.2 在 set -u 下展开空数组会报 unbound variable，
  # QT_DEPLOY_LIB_PATHS 在初始化时至少有一个元素，因此展开不会触发该问题。
  if [[ "$DEBUG_BUILD" == "1" ]]; then
    deploy_args+=(-no-strip)
  else
    :
  fi
  local deploy_log
  deploy_log="$(mktemp "${TMPDIR:-/tmp}/quizpane-macdeployqt.XXXXXX")"
  set +e
  "${deploy_args[@]}" 2>&1 | tee "$deploy_log"
  local deploy_status="${PIPESTATUS[0]}"
  set -e
  if [[ "$deploy_status" -ne 0 ]] || grep -q '^ERROR:' "$deploy_log"; then
    echo "macdeployqt 部署失败：$app_path" >&2
    rm -f "$deploy_log"
    return 1
  fi
  rm -f "$deploy_log"
}

# Homebrew 的 macdeployqt 在 Qt 模块拆分安装时不会始终解析 QtSvg/QtPdf 的
# rpath；这两个 Framework 是当前 UI 和 PDF 提取的直接依赖，因此明确复制，
# 后续仍由外层 codesign 统一签名。
copy_qt_framework() {
  local app_path="$1"
  local framework_path="$2"
  if [[ ! -d "$framework_path" ]]; then
    echo "缺少 Qt Framework：$framework_path" >&2
    exit 1
  fi
  mkdir -p "$app_path/Contents/Frameworks"
  local destination="$app_path/Contents/Frameworks/$(basename "$framework_path")"
  ditto "$framework_path" "$destination"
  chmod -R u+w "$destination"
}
# macdeployqt 会把 staging 根目录的 lib 当成一个隐式搜索目录。把 Homebrew
# Framework 暂时暴露到这里，让它自行复制进 App，避免和预复制的目标文件冲突。
mkdir -p "$STAGE_ROOT/lib"
ln -s "$QT_SVG_FRAMEWORK" "$STAGE_ROOT/lib/QtSvg.framework"
ln -s "$QT_PDF_FRAMEWORK" "$STAGE_ROOT/lib/QtPdf.framework"
deploy_app "$STAGED_APP"
STAGED_STUDIO="$STAGE_ROOT/QuizPaneQuestionMaker.app"
ditto "$SOURCE_STUDIO" "$STAGED_STUDIO"
if [[ ! -f "$TESSDATA_DIR/eng.traineddata" || ! -f "$TESSDATA_LANG_DIR/chi_sim.traineddata" ]]; then
  echo "缺少 OCR 语言数据：$TESSDATA_DIR/eng.traineddata 或 $TESSDATA_LANG_DIR/chi_sim.traineddata" >&2
  exit 1
fi
mkdir -p "$STAGED_STUDIO/Contents/Resources/tessdata"
cp "$TESSDATA_LANG_DIR/chi_sim.traineddata" "$TESSDATA_DIR/eng.traineddata" \
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
rm -rf "$STAGE_ROOT/lib"
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
