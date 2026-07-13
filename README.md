<div align="center">
  <img src="apps/desktop-qt/resources/app-icon.png" width="128" alt="QuizPane 小窗刷题图标">
  <h1>小窗刷题 · QuizPane</h1>
  <p><strong>把题目贴在桌面角落，随时做两道，随时一键收起。</strong></p>
  <p>透明悬浮 · 老板键 · 草稿恢复 · 多题库 · 原生低占用</p>
  <p>
    <a href="https://github.com/tianyoudoge/quizpane/releases/latest">下载最新版</a> ·
    <a href="#安装和使用">安装教程</a> ·
    <a href="#题库怎么安装">题库教程</a> ·
    <a href="#developer-guide">Developer Guide</a>
  </p>
</div>

---

## 这是什么？

小窗刷题是一款放在桌面右上角的轻量刷题工具。它不像传统题库客户端那样占满整个屏幕，而是尽量保持成一张低存在感的小卡片：工作间隙做一道，窗口不用来回切；有人过来时，按一下老板键立即隐藏。

它不绑定某一家题库。你可以安装不同来源的题库文件，在同一个小窗里切换、答题、交卷和看解析。

完整发行版还包含独立“题库生成器”：通过四步图形向导导入自己的 TXT、Word、PDF 或 JSON，查看整理进度和 Token 用量，最终生成跨平台本地题库。云端或本地模型在应用“设置”菜单中统一配置。

> 从 `v0.2.0` 开始，Release 同时提供 Windows 10/11 x64、macOS Apple Silicon、macOS Intel 和 Linux x64 构建，并单独提供“题库生成器”安装包。Linux 包面向统信 UOS、银河麒麟等桌面发行版进行兼容构建；题库市场仍在开发中。

## 界面长什么样？

<div align="center">
  <img src="docs/images/quizpane-catalog.png" width="380" alt="小窗刷题选择练习界面">
  <p><sub>公开 Demo 题库实拍：选择分类和题量，点击即可开始练习。</sub></p>
</div>

整个界面只有三层：

1. **标题栏**：切换题库、调整大小、PIN 置顶和关闭；
2. **答题区**：显示题目、选项、正确答案和解析，内容过长时单独滚动；
3. **操作栏**：上一题、下一题和交卷，始终固定在底部。

窗口没有传统标题边框，背景使用深色透明效果。题目图片会尝试去除大面积纯白背景，减少白色图片突然照亮屏幕的情况。

## 为什么适合放在桌面角落？

| 功能 | 使用体验 |
|---|---|
| 透明悬浮窗 | 不遮住整个桌面，拖到任意角落即可使用 |
| PIN 置顶 | 写文档、看网页时仍保持在其他窗口上方 |
| 老板键 | 默认 `Ctrl+Shift+H`，一键隐藏，再按一次恢复 |
| 三档尺寸 | 小、中、大随时切换，兼顾隐蔽性和阅读体验 |
| 固定操作栏 | 长题只滚动正文，上一题、下一题和交卷不会跑掉 |
| 自动草稿 | 关闭或崩溃后重新打开，可以继续上次没做完的题 |
| 多题库 | 题库之间独立保存登录状态和草稿，切换不会互相污染 |
| 原生客户端 | C++/Qt Widgets 实现，没有浏览器壳，适合低配置电脑 |

## 安装和使用

### 先分清两个程序

每个平台都提供两个独立下载包，不需要两个都安装：

| 你要做什么 | 下载文件前缀 | 解压后的程序 |
|---|---|---|
| 在桌面小窗里刷题 | `QuizPane-*` | 小窗刷题 |
| 把 TXT、Word、PDF、JSON 整理成题库 | `QuizPane-Bank-Studio-*` | 题库生成器 |

题库生成器的输出是 `.quizpane-provider` 文件。生成完成后，再用“小窗刷题”导入；只想使用别人提供的题库时，无需下载题库生成器。

### 按系统选择下载文件

| 系统 | 小窗刷题 | 题库生成器 |
|---|---|---|
| macOS Apple Silicon（M1/M2/M3/M4） | `QuizPane-macos-arm64.zip` | `QuizPane-Bank-Studio-macos-arm64.zip` |
| macOS Intel | `QuizPane-macos-x86_64.zip` | `QuizPane-Bank-Studio-macos-x86_64.zip` |
| Windows 10/11 64 位 | `QuizPane-windows-x64.zip` | `QuizPane-Bank-Studio-windows-x64.zip` |
| Linux/UOS/银河麒麟 64 位 | `QuizPane-linux-x86_64.tar.gz` | `QuizPane-Bank-Studio-linux-x86_64.tar.gz` |

所有文件都在 [最新版 Releases](https://github.com/tianyoudoge/quizpane/releases/latest)。Windows 7、Linux ARM64、macOS 10.14/10.15 当前暂不支持，请勿下载名称相近的其他架构包尝试覆盖安装。

### macOS 安装

1. 打开 [Releases](https://github.com/tianyoudoge/quizpane/releases/latest)；
2. 根据处理器和用途，从上表选择 ARM64 或 x86_64 的主程序/题库生成器；
3. 解压后，把对应 `.app` 拖入“应用程序”；
4. 第一次启动时，右键应用并选择“打开”；
5. 小窗刷题负责安装题库，题库生成器负责创建题库，二者可以独立启动。

当前自动构建版本使用临时签名，尚未完成 Apple Developer ID 公证。如果 macOS 阻止运行，请先确认文件来自本仓库的正式 Release，并核对同页面提供的 SHA-256；不要关闭整台电脑的 Gatekeeper。

校验下载文件：

```bash
shasum -a 256 <下载的文件名>
```

### Windows 安装

下载对应 `.zip` 后解压到普通文件夹：运行“小窗刷题.exe”开始刷题，或运行“题库生成器.exe”创建题库。两个压缩包已经分别携带所需 Qt 运行库，不要只复制其中的 `.exe` 到别处。

### Linux、统信 UOS、银河麒麟安装

下载对应 `.tar.gz` 并完整解压。主程序运行 `QuizPane.AppDir/AppRun`，题库生成器运行 `QuizPane-Bank-Studio.AppDir/AppRun`。不要把 `usr/bin` 中的单个文件拿出来运行；Qt 插件和运行库位于同一个 AppDir 中。当前属于 Linux x64 兼容构建，并非统信或麒麟官方认证版本。

## 题库怎么安装？

小窗刷题使用扩展名为 `.quizpane-provider` 的题库文件。可以把它理解成播放器的“内容源组件”。

### 方法一：从菜单添加

1. 点击右上角“三横线”；
2. 选择“题库管理”；
3. 点击“添加题库…”；
4. 选择 `.quizpane-provider` 文件；
5. 检查题库名称、版本和权限，确认后安装。

### 方法二：直接拖入

把 `.quizpane-provider` 文件拖到小窗上，确认信息后即可安装。

### 切换或删除题库

```text
三横线
└── 题库管理
    ├── 添加题库…
    ├── 创建题库…
    ├── 切换题库
    └── 删除题库
```

删除题库组件时，默认保留它的登录状态和未完成草稿。以后重新安装同一个题库，可以继续使用；退出账号或彻底删除数据的能力会在后续题库管理版本补齐。

> 联网原生 Provider 可能包含本机代码，安装它相当于运行第三方软件。只安装来源可信、签名有效、具有合法内容授权的原生题库。由题库生成器产生的声明式本地题库只包含 JSON 和图片，不执行第三方代码。

## 从选题到看解析

### 1. 选择练习

打开题库后，选择分类和题量。远程题库可能先要求使用对应手机 App 扫描二维码，本地题库通常可以直接进入分类。

### 2. 做题

点击选项完成选择。顶部显示当前题号，底部左右三角切换上一题和下一题。题目太长时，用鼠标滚轮滚动中间答题区。

### 3. 自动保存

选择答案后会增量保存。关闭小窗或电脑重启后，再次进入同一个题库会提示恢复草稿。

### 4. 交卷

点击底部文稿形状的交卷图标，确认后提交。交卷完成会显示题目数量、作答数和正确数。

### 5. 查看解析

交卷后逐题显示：

- 你的选择；
- 正确答案；
- 题目解析；
- 上一题、下一题。

题干和解析使用不同的低饱和颜色，避免内容混在一起。

## 悬浮、尺寸和老板键

### PIN 置顶

点击标题栏图钉。高亮时窗口保持在其他应用上方；再次点击取消置顶。

### 调整尺寸

点击标题栏的缩放图标，选择小、中、大。上下调整窗口时主要改变答题区域高度，标题栏和操作栏保持稳定。

### 设置老板键

打开“三横线 → 老板键设置…”，按下新的组合键并保存。快捷键必须包含 Ctrl、Shift、Alt、Cmd/Win 中至少一个修饰键。

默认老板键：

```text
Ctrl + Shift + H
```

老板键在系统全局生效，小窗不在焦点时也可以隐藏或恢复。如果组合键已被其他软件占用，程序会提示并保留原设置。

## 当前版本已经有什么？

- [x] 透明无边框悬浮窗、拖动、PIN 和三档尺寸
- [x] 老板键、系统托盘和 macOS 顶部菜单
- [x] 添加、切换和删除多个题库
- [x] 二维码登录界面和系统安全凭据存储
- [x] 分类、组卷、答题、草稿、交卷和逐题解析
- [x] 长题滚动、固定操作栏和低干扰线条图标
- [x] 题图近白背景透明化与内容区域裁剪
- [x] 完全离线 Demo Provider 和自动测试
- [x] 声明式跨平台题库运行时与题库生成器四步 UI
- [ ] 官方题库市场
- [ ] 本地题库生成器模型调用、文档提取与任务恢复
- [x] Windows、Intel Mac、Linux x64 自动构建与 Release

## 常见问题

### 为什么安装后没有题目？

QuizPane 是通用刷题 Host，题目来自单独安装的题库组件。当前题库市场仍在开发，需要自行导入可信的 `.quizpane-provider`。

### 关闭软件会退出登录吗？

不会。登录凭据由 macOS Keychain、Windows Credential Manager 或 Linux Secret Service 保存，并按题库隔离。

### 为什么不内置某个商业平台的题目？

公开仓库不分发未经授权的第三方协议和题库内容。官方索引将只接收可以证明接口与内容授权的 Provider。

### 这是开源软件吗？

源码公开可读，但许可证是 PolyForm Noncommercial 1.0.0：个人和符合定义的非商业使用免费，商业使用需要单独授权。严格意义上属于 **source-available（源码可用）**，不是 OSI 定义的开源许可证。

---

<a id="developer-guide"></a>

# 开发者文档

普通用户读到这里就够了。下面是源码构建、Provider 开发和平台贡献说明。

## 工程结构

```text
quizpane/
├── apps/desktop-qt/       # Qt Widgets 桌面 Host
├── core/                  # 答题、草稿和图片处理领域逻辑
├── sdk/                   # Provider ABI、加载器和安装器
├── providers/demo/        # 完全离线的示例题库
├── tools/bank-generator/  # 本地题库生成器（开发中）
├── tests/                 # 自动测试
├── packaging/             # 平台打包资源
├── scripts/               # macOS/Windows/Linux 构建脚本
└── docs/                  # 架构与构建文档
```

需要 CMake 3.24+、Ninja、C++20 编译器和 Qt 6.5+，包含 Core、Widgets、Network 及匹配版本的 Qt Core private headers。

## macOS 开发构建

```bash
brew install cmake ninja qtbase
git clone git@github.com:tianyoudoge/quizpane.git
cd quizpane
export CMAKE_PREFIX_PATH="$(brew --prefix qtbase)"
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

运行公开 Demo Provider：

```bash
./build/dev/apps/desktop-qt/小窗刷题.app/Contents/MacOS/小窗刷题 \
  --provider ./build/dev/providers/demo/libquizpane_provider_demo.dylib
```

生成 macOS 发布包：

```bash
./scripts/build-macos.sh
```

## Windows 10/11 构建

在 Visual Studio 2022 x64 Developer Prompt 中：

```powershell
git clone git@github.com:tianyoudoge/quizpane.git
cd quizpane
.\scripts\build-windows.ps1 -QtRoot C:\Qt\6.8.0\msvc2022_64
```

Windows 7 不使用当前 Qt 6 主线，需要单独的 Qt 5.15 兼容分支。

## UOS 与银河麒麟构建

```bash
sudo apt install build-essential cmake ninja-build pkg-config \
  qt6-base-dev qt6-base-private-dev libx11-dev libsecret-1-dev
```

UOS：

```bash
./scripts/build-uos.sh
```

银河麒麟：

```bash
DISTRO_ID=kylin DIST_DIR="$PWD/dist/kylin" ./scripts/build-uos.sh
```

Linux 可以共用较老的 ABI 构建基线，但 x86_64、ARM64 必须分别编译，并在每一个声明支持的目标系统上真机验收。详细说明见 [`docs/题库管理与跨平台构建打包指南.md`](docs/题库管理与跨平台构建打包指南.md)。

## Provider 开发

Provider 使用稳定 C ABI，Host 与 Provider 通过 UTF-8 JSON RPC 交互。最小实现参考 [`providers/demo`](providers/demo) 和 [`docs/架构与Provider开发导读.md`](docs/架构与Provider开发导读.md)。

公开 Provider 必须：

- 拥有接口、题目、图片和解析的必要授权；
- 声明网络、安全凭据和本地文件权限；
- 不读取其他 Provider 的凭据；
- 不包含 Cookie、Token、抓包或无授权内容；
- 分别为目标操作系统和 CPU 架构构建。

## 题库生成器（开发中）

当前可以校验题库源 JSON：

```bash
cmake -S . -B build/dev -G Ninja \
  -DCMAKE_PREFIX_PATH=/path/to/Qt \
  -DQUIZPANE_BUILD_BANK_GENERATOR=ON
cmake --build build/dev --target quizpane-bank-generator
./build/dev/tools/bank-generator/quizpane-bank-generator \
  --validate tools/bank-generator/examples/example-bank.json
```

CSV/QTI 导入、`.quizpane-bank` 分块压缩、签名、加密和图形化向导仍在开发。

## 参与平台构建

我们需要 Intel macOS、Windows 10/11、统信 UOS 和银河麒麟用户帮助构建。提交产物时必须同时提供：

- 对应 Git commit；
- 操作系统、CPU、编译器、Qt 和系统库版本；
- 完整构建命令与测试输出；
- 干净系统启动验收结果；
- SHA-256。

维护者不会直接发布来源不明的二进制。完整流程见 [`CONTRIBUTING.md`](CONTRIBUTING.md)，安全问题见 [`SECURITY.md`](SECURITY.md)。

## English

QuizPane is a compact, native desktop quiz window designed to stay in a corner instead of taking over the screen. It supports translucent always-on-top UI, a global hide/show hotkey, draft recovery, multiple question-bank Providers, submission, and per-question explanations.

### Install

Each platform has two separate downloads: `QuizPane-*` is the floating quiz client, while `QuizPane-Bank-Studio-*` is the optional question-bank creator. Choose `macos-arm64` for Apple Silicon, `macos-x86_64` for Intel Macs, `windows-x64` for 64-bit Windows 10/11, or `linux-x86_64` for Linux/UOS/Kylin. Download them from the [latest release](https://github.com/tianyoudoge/quizpane/releases/latest).

The Bank Studio creates `.quizpane-provider` files that can then be imported into QuizPane. Users who only install existing question banks do not need Bank Studio. Windows 7, Linux ARM64, and macOS 10.14/10.15 are not supported by the current release.

The current build is an unnotarized technical preview. Verify the SHA-256 published with the release and do not disable Gatekeeper globally.

### Use

1. Add a trusted question-bank Provider;
2. choose a category and question count;
3. answer questions in the floating window;
4. submit the attempt;
5. review the correct answer and explanation;
6. press `Ctrl+Shift+H` to hide or restore the window globally.

### Build

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

See the Chinese developer sections above, [`CONTRIBUTING.md`](CONTRIBUTING.md), and [`docs/题库管理与跨平台构建打包指南.md`](docs/题库管理与跨平台构建打包指南.md) for full platform instructions.

## 许可证 / License

QuizPane 使用 [PolyForm Noncommercial License 1.0.0](LICENSE)。个人及符合定义的非商业用途免费；广告版、企业集成、收费发行和其他商业用途需要书面授权。联系 **xutianyoubupt@foxmail.com**，详见 [`COMMERCIAL_LICENSE.md`](COMMERCIAL_LICENSE.md)。

QuizPane is licensed under the [PolyForm Noncommercial License 1.0.0](LICENSE). Qualifying personal and noncommercial use is free. Commercial use requires a separate written license; see [`COMMERCIAL_LICENSE.md`](COMMERCIAL_LICENSE.md).

Third-party dependencies retain their own licenses. The QR encoder is based on Project Nayuki's MIT-licensed QR Code generator.
