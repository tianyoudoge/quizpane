# QtPdf 5.15.2 公共头文件（vendored）

官方 Qt 5.15.2 Windows 二进制发行包（`qtwebengine.win64_msvc2019_64`）包含
`Qt5Pdf.dll` / `Qt5Pdf.lib`（pdfium 已静态链接进 Qt5Pdf.dll），但**不附带**
`include/QtPdf` 头文件和 `lib/cmake/Qt5Pdf` 配置。因此从 qtwebengine 源码
v5.15.2 的 `src/pdf/api/` 逐字镜像公共头文件到这里，供 CMake 以 imported
target 方式链接官方预编译库。

- 来源：https://code.qt.io/qt/qtwebengine.git，tag `v5.15.2`，目录 `src/pdf/api/`
- 许可：LGPL-3.0（源文件自带 Qt 的 LGPL3/GPL2 双许可头，未改动正文）
- 本目录只镜像被本项目引用的 API 头：`qpdfdocument.h`、`qpdfselection.h`、
  `qpdfdocumentrenderoptions.h`、`qpdfnamespace.h`、`qtpdfglobal.h`，以及
  两个 Qt 风格的驼峰转发头 `QPdfDocument` / `QPdfSelection`
- 需要的库文件 `Qt5Pdf.dll` / `Qt5Pdf.lib` 由 CI 从官方源安装（见
  `.github/workflows/windows7-desktop.yml` 的 qtwebengine 模块安装步骤），
  本地构建需自行准备同版本 Qt 并放入对应文件

升级 Qt 小版本时，同步更新本目录头文件并核对 qtwebengine tag。
