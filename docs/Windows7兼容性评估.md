# Windows 7 兼容性实现与验收结论

> 更新日期：2026-08-14；实现分支：`refeactor_win7_support`；目标系统：Windows 7 SP1 x64。

## 当前结论

仓库已具备一条 **Qt 5.15.2 + MSVC 2019/v142** 的 Win7 兼容构建线，同时保留原有 Qt 6 构建。Qt 6 发行包仍然只支持 Windows 10/11，不能通过 Windows“兼容模式”运行于 Win7。

这次改造证明：项目没有必须依赖 Qt 6 的核心业务功能。Qt 版本依赖主要来自原 CMake 的硬编码和少量 API 命名变化。当前完成的是“源码与构建链兼容”，并已通过本机 Qt 6 全量回归；由于尚未在干净 Win7 SP1 VM 上运行产物，现阶段不能把结论写成“已完成 Win7 真机认证”。

## 已完成的改造

- CMake 可由 `QUIZPANE_QT_MAJOR_VERSION=5|6` 选择 Qt 大版本；`QUIZPANE_WINDOWS7_COMPAT=ON` 强制 Qt 5、MSVC 与 Win7 SDK 宏。
- Win7 构建固定为 Qt 5.15.2、MSVC 2019/v142，并给两个 EXE 合并 Windows 7 `supportedOS` manifest。
- 新增 `scripts/build-windows7.ps1`，默认生成 `QuizPane-windows7-x64-portable.zip`。
- 自动更新在 Win7 构建中只查找 `windows7-x64` 资产，不会误装 Windows 10/11 的 Qt 6 包。
- 网课伴侣清单基线降至 Chrome/Edge 109；Windows 使用真实浏览器 popup，Chrome 116 专属的 macOS tabCapture 实验在旧浏览器中通过能力检测安全跳过。
- CI 新增 Qt 5.15/v142 编译测试；另有手工触发的 Win7 兼容包工作流。
- Qt 6 主线仍按原方式构建，不需要维护两套业务源码。

### 实际 Qt 5/Qt 6 API 差异

| 范围 | Qt 6 | Qt 5.15 | 处理 |
| --- | --- | --- | --- |
| 鼠标全局坐标 | `globalPosition()` | `globalPos()` | 版本条件封装 |
| 鼠标局部坐标 | `position()` | `localPos()` | 版本条件封装 |
| QVariant 类型 | `metaType().id()` | `type()` | 版本条件判断 |
| Windows 原生事件过滤器 | 结果参数为 `qintptr*` | 结果参数为 `long*` | 版本条件签名，保留全局热键 |
| 严格 UTF-8 解码 | `QStringDecoder` | `QTextCodec::ConverterState` | 保留失败后回退 GB18030 的语义 |
| PDF 加载成功枚举 | `QPdfDocument::Error::None` | `QPdfDocument::NoError` | 小型兼容函数 |
| PDF 页面尺寸 | `pagePointSize()` | `pageSize()` | 小型兼容函数 |

特别澄清：Qt 5.15 的 `QPdfDocument` 同样提供同步 `load(QString)` 错误码、`pageCount()`、`pageSize()`、`render()`、`getAllText()` 和 `getSelectionAtIndex()`。它不是“只有状态 API”，也不需要自行计算 PDF 页面尺寸；题库制作器的 PDF 主流程可以直接复用。

## 功能边界

| 功能 | 当前判断 |
| --- | --- |
| 离线题库、答题、草稿恢复、全局热键、置顶、文件关联 | 代码与依赖已进入 Win7 兼容构建，待 Win7 VM 冒烟 |
| 题库制作器 TXT、DOCX、PDF、AI 联网 | 已完成 Qt 5 源码适配，待 Win7 VM 回归与 TLS 验证 |
| OCR | Win7 包默认关闭；Tesseract/vcpkg 依赖尚未固定并在 Win7 验收，可用 `-EnableOcr` 做实验构建 |
| 网课伴侣 | 已建立 Chrome/Edge 109 兼容基线：Windows 使用真实浏览器 popup，并由桌面端通过 Win32 置顶/隐藏/恢复；待 Win7 VM 回归 |

Qt 5.15 已结束常规支持，Windows 7 也已停止安全支持。因此兼容包应作为独立产物维护，不应替换 Windows 10/11 的 Qt 6 正式包。

## 构建方法

准备 Qt 5.15.2 的 `msvc2019_64` 套件，并安装 `qtwebengine` 模块（Qt 5 的 QtPdf 随该模块提供）。在 Visual Studio 2019 x64 Developer Prompt 中执行：

```powershell
.\scripts\build-windows7.ps1 -QtRoot C:\Qt\5.15.2\msvc2019_64
```

脚本会配置、编译、执行测试、调用 `windeployqt`，最后生成：

```text
dist/windows7/QuizPane-windows7-x64-portable.zip
```

GitHub Actions 的 `Build Windows 7 compatibility package` 工作流可手工生成桌面测试包与 `QuizPane-course-companion-chrome109.zip`。Hosted Runner 只能验证 Qt 5/v142 构建和扩展自动测试，不能替代 Win7/Chrome 109 运行验收。

## 发布前必须通过的门槛

1. 在干净 Windows 7 SP1 x64 VM 完整解压绿色包，确认两个 EXE 均可启动且不依赖开发机环境。
2. 检查 EXE/DLL 导入表，确认没有 Win7 缺失的系统 API；安装所需 UCRT/VC 运行库更新后再测试。
3. 回归题库安装、答题、草稿恢复、文件关联、全局热键、置顶、TXT/DOCX/PDF 导入与题库导出。
4. 验证 Qt Network 在 Win7 上访问项目 HTTPS 服务的 TLS 与证书链；不得为兼容性关闭证书校验。
5. 在没有兼容显卡驱动的 VM 中验证 Qt 软件渲染降级，避免启动白屏或平台插件失败。
6. 用 Chrome/Edge 109 回归绑定、真实 popup、置顶、播放控制、老板键隐藏/恢复和标签页归还；发布说明同时标注 Win7 已停止安全支持。

只有完成上述 VM 验收后，结论才能从“可构建的兼容候选版”升级为“Win7 可用版”。

## 依据

- [Qt 5.15 Windows 支持矩阵（归档）](https://doc.qt.io/archives/qt-5.15/windows.html)
- [Qt 6.5 Windows 支持矩阵](https://doc.qt.io/qt-6.5/windows.html)
- [Microsoft UCRT 部署说明](https://learn.microsoft.com/en-us/cpp/windows/universal-crt-deployment?view=msvc-170)
- [Chrome 企业版系统要求](https://support.google.com/chrome/a/answer/7100626) 与 [Microsoft Edge 生命周期](https://learn.microsoft.com/en-us/lifecycle/products/microsoft-edge)
