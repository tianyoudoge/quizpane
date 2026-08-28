# Win7 离线 OCR 验证包

状态：实验性接入；**未经过 Win7 SP1 实机验收，不提升正式包的兼容性承诺**。

## 构建与依赖

在 MSVC 2019/v142 14.29 对应架构开发者终端执行：

```powershell
./scripts/build-windows7.ps1 -EnableOcr -Architecture x64 -QtRoot C:/Qt/5.15.2/msvc2019_64
./scripts/build-windows7.ps1 -EnableOcr -Architecture x86 -QtRoot C:/Qt/5.15.2/msvc2019
```

两条命令须分别在 x64/x86 终端执行。默认依赖缓存、应用构建和产物目录隔离。
依赖源码、模型固定版本并校验 SHA-256，许可及来源随包放在 `licenses/`。
Tesseract/Leptonica 使用 `/MD`，与应用共享随包 v142 CRT；静态链接不代表没有 CRT 依赖。
只保留 RGB 内存输入，不需要 Leptonica 的 PNG/JPEG/TIFF 解码库；PDF 渲染仍由 Qt5Pdf 完成。
上游 x86 SIMD 检测被显式禁用，避免高阶指令和实验 OpenMP 构建参数。

## 自动验证

- `windows7-desktop.yml` 的 x64/x86 Qt5/v142 打包测试默认启用 OCR；手动可关闭。
- `document_extractor_test` 检查真实的无文字层 PDF，断言读到固定题干/选项文字。
- 解压测试在中文路径下运行，清空 tessdata/Qt 插件环境变量，只使用包内模型和运行库。
- 损坏中文模型时必须明确失败，不能回退到英文识别，也不能崩溃后算作测试通过。
- 检查两个 EXE 的架构和常见 Win8+ API 导入。此检查不是完整系统 API 白名单验证。
- 可读封面加无法 OCR 的正文，不再只报告“没有识别到题号锚点”。

Hosted Runner 是 Windows Server 2022，成功只代表构建、部署及该系统上的 OCR 冒烟通过。
macOS 上依赖源码构建及 Qt6 回归只能验证共享逻辑，不能验证 Win7 ABI。

### 本次本地验证（2026-08-28，macOS arm64）

- Qt6 Release 原有 28 项测试通过；新增 Win7 OCR CMake 回归后，关闭 OCR 的 CI 配置
  全量 29 项测试通过。
- 固定的 Tesseract/Leptonica 精简静态库完成源码构建，并链接提取器执行扫描 PDF、
  中文模型路径、损坏中文模型测试，均通过。
- 关闭 OCR、保留 QtPdf 的提取器与规则测试通过；无 QtPdf/OCR 配置的相关测试也通过。
- 四个 PowerShell 脚本通过真实 PowerShell 语法解析；解压目录的模型测试脚本在本机
  使用上述原生测试程序通过了正向/损坏模型检查。未执行 Windows PE 导入检查。
- Windows Server 2022 [GitHub 工作流 #33142659825](https://github.com/tianyoudoge/quizpane/actions/runs/33142659825)
  的 x64/x86 Qt5/v142 构建、29 项测试、中文路径 ZIP OCR、损坏模型和导入检查均通过，
  已生成两种架构的 `ocr-test` Artifact。**尚未进行 Win7 SP1 实机测试。**

## 必须补充的 Win7 SP1 验收

1. 分别用干净的 Win7 SP1 x64/x86 环境，完整解压到包含中文和空格的路径。
2. 无开发工具、无系统 OCR、断网启动两个应用，确认没有缺 DLL/入口点/非法指令错误。
3. 导入仓库扫描夹具与中文扫描 PDF，确认能提取题干，记录时间、峰值内存和明显错字。
4. 导入可读封面加转曲正文的题本，确认正文进入 OCR，而不是只读取封面。
5. 用 165 页题本观察长时间运行和 32 位内存占用；检查取消/关闭时的应用行为。
6. 移走或损坏模型，确认提示“识别模型缺失或损坏”，完整恢复后可再次识别。
7. 回归普通文字 PDF、TXT、DOCX；复杂题本的答案表/分组/共享材料正确性另行验收。

通过上述验收前，不启用正式 Release 的 Win7 OCR 默认开关，不自动合并或发版。
