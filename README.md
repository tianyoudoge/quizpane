# 小窗刷题 · QuizPane

> 一个原生、轻量、可扩展的桌面刷题小窗。  
> A native, lightweight, extensible desktop quiz window.

[中文](#中文) · [English](#english) · [许可证](#许可证--license)

## 中文

QuizPane 把刷题界面压缩成一个可置顶、可拖动、可缩放的透明小窗。Host 不绑定任何具体题库：题目由用户安装的“题库组件”（Provider）提供，本地题库和获得合法授权的远程题库可以使用同一套答题体验。

> 本公开仓库不包含任何第三方平台私有协议、账号凭据、抓包或未经授权的题目内容。

### 主要界面

#### 1. 题库与登录

安装题库后，QuizPane 根据组件能力显示本地分类或二维码登录。登录凭据按题库隔离，保存在 macOS Keychain、Windows Credential Manager 或 Linux Secret Service 中。

#### 2. 悬浮答题

- 无边框透明/毛玻璃背景；
- PIN 一键置顶；
- 小、中、大三档 UI；
- 固定标题栏和操作栏，长题仅滚动答题区；
- 低识别度线条按钮、题干与解析低饱和分色；
- 近白底题图透明化和内容区域裁剪。

#### 3. 交卷与解析

完成选择后交卷，可逐题查看正确答案和解析。草稿会原子保存，意外退出或重启后可以恢复。

#### 4. 题库管理

右上角三横线菜单支持：

```text
题库管理
├── 添加题库…
├── 切换题库
└── 删除题库
老板键设置…
关于小窗刷题
```

### 安装题库

题库组件扩展名为 `.quizpane-provider`：

1. 打开“三横线 → 题库管理 → 添加题库…”；或
2. 直接把 `.quizpane-provider` 拖入小窗；
3. 检查发布者、平台、版本和权限后确认安装；
4. 从“切换题库”选择它。

Provider 是原生代码，只安装可信来源、签名有效且具有合法内容授权的组件。删除组件默认不会删除该题库的登录态和草稿。

### 功能状态

- [x] 透明无边框、置顶、拖动和三档尺寸
- [x] 老板键、托盘和 macOS 顶部菜单
- [x] 多题库添加、切换、删除
- [x] 二维码登录与跨平台安全凭据接口
- [x] 分类、组卷、答题、草稿、交卷、解析
- [x] 稳定 C ABI + UTF-8 JSON Provider SDK
- [x] Demo Provider 和自动测试
- [ ] 本地题库生成器（已提供 JSON 校验骨架，打包/签名/加密开发中）
- [ ] Provider 签名信任链和社区市场

### 支持平台

| 平台 | 目标架构 | 当前状态 |
|---|---|---|
| macOS | Apple Silicon | 已完成本机编译和自动测试 |
| macOS | Intel | 构建路径已提供，等待目标机验证 |
| Windows 10/11 | x64 | 构建脚本已提供，等待社区验证 |
| Windows 7 | x64 | 需要独立 Qt 5.15 兼容分支 |
| 统信 UOS | x86_64 / ARM64 | Linux 构建入口已提供，等待真机验证 |
| 银河麒麟 | x86_64 / ARM64 | Linux 构建入口已提供，等待真机验证 |

不同 CPU 架构必须分别构建 Host 和 Provider。Linux 可以共用较老的 ABI 构建基线，但仍需在每个目标系统真机验收。

### 用户构建与安装

需要 CMake 3.24+、Ninja、C++20 编译器和 Qt 6.5+（Core、Widgets、Network 以及对应 private headers）。

macOS：

```bash
brew install cmake ninja qtbase
git clone <your-fork-or-repository-url> quizpane
cd quizpane
export CMAKE_PREFIX_PATH="$(brew --prefix qtbase)"
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
./scripts/build-macos.sh
```

Windows 10/11（VS 2022 x64 Developer Prompt）：

```powershell
git clone <your-fork-or-repository-url> quizpane
cd quizpane
.\scripts\build-windows.ps1 -QtRoot C:\Qt\6.8.0\msvc2022_64
```

UOS：

```bash
sudo apt install build-essential cmake ninja-build pkg-config \
  qt6-base-dev qt6-base-private-dev libx11-dev libsecret-1-dev
./scripts/build-uos.sh
```

银河麒麟：

```bash
DISTRO_ID=kylin DIST_DIR="$PWD/dist/kylin" ./scripts/build-uos.sh
```

详细依赖、打包和验收矩阵见 [`docs/题库管理与跨平台构建打包指南.md`](docs/题库管理与跨平台构建打包指南.md)。

### 开发者选项

#### 开发 Provider

Provider 使用稳定 C ABI，Host 与插件通过 UTF-8 JSON RPC 交互，类似进程内的小型 HTTP API。最小实现可参考 [`providers/demo`](providers/demo)。

```bash
cmake --preset dev
cmake --build --preset dev --target quizpane_provider_demo
ctest --preset dev
```

公开 Provider 必须：

- 拥有接口与题库内容的必要授权；
- 声明网络、安全存储和本地文件权限；
- 不读取其他 Provider 的凭据；
- 不包含真实 Cookie、Token、抓包或第三方私有内容；
- 分别为每个操作系统和 CPU 架构构建。

#### 题库生成器（开发中）

```bash
cmake -S . -B build/dev -G Ninja \
  -DCMAKE_PREFIX_PATH=/path/to/Qt \
  -DQUIZPANE_BUILD_BANK_GENERATOR=ON
cmake --build build/dev --target quizpane-bank-generator
./build/dev/tools/bank-generator/quizpane-bank-generator \
  --validate tools/bank-generator/examples/example-bank.json
```

当前只校验源 JSON；`.quizpane-bank` 的分块压缩、签名、加密和图形化导入仍在开发。参见 [`tools/bank-generator`](tools/bank-generator)。

### 帮助我们提供其他平台安装包

我们需要 Intel macOS、Windows、UOS 和银河麒麟用户参与构建验证。不要只上传一个无法追溯的压缩包；请同时提供：

- 对应 Git commit；
- 操作系统、CPU、编译器、Qt 与系统库版本；
- 完整构建命令和测试输出；
- 干净系统启动验收结果；
- 产物 SHA-256。

维护者复核来源与可复现性后再发布正式产物。流程见 [`CONTRIBUTING.md`](CONTRIBUTING.md)。

### 工程结构

```text
quizpane/
├── apps/desktop-qt/       # Qt Widgets 桌面 Host
├── core/                  # 答题、草稿、图片隐私化领域逻辑
├── sdk/                   # Provider ABI、加载器和安装器
├── providers/demo/        # 完全离线的示例题库
├── tools/bank-generator/  # 本地题库生成器（开发中）
├── tests/                 # 自动测试
├── packaging/             # 平台打包资源
├── scripts/               # macOS/Windows/Linux 构建脚本
└── docs/                  # 设计与开发文档
```

## English

QuizPane turns quiz practice into a compact, always-on-top, draggable, resizable transparent desktop window. The host is source-neutral: local banks and legitimately authorized remote services integrate through native Provider packages and share one practice workflow.

This public repository contains no private third-party protocols, credentials, packet captures, or unauthorized question content.

### Main experiences

- **Bank and sign-in:** a Provider may expose local categories directly or request QR sign-in. Credentials are isolated per Provider and stored through the operating system's secure credential service.
- **Floating practice:** borderless translucent UI, pin-to-top, three sizes, compact fixed toolbars, a scrollable question body, low-profile line icons, draft recovery, and white-background image cleanup.
- **Submit and review:** answer questions, submit the attempt, then inspect the correct answer and explanation one question at a time.
- **Bank management:** add, switch, and remove `.quizpane-provider` packages from the menu or by drag and drop.

Only install Providers from trusted publishers with valid rights to their APIs and content. A Provider is native code and has the same local execution risk as any other plugin.

### Build

Requirements: CMake 3.24+, Ninja, a C++20 compiler, and Qt 6.5+ with Core, Widgets, Network, and matching private headers.

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Platform packaging scripts live in [`scripts`](scripts). macOS Apple Silicon is currently tested locally; Intel macOS, Windows 10/11, UOS, and Kylin need reproducible community build reports. Windows 7 requires a separate Qt 5.15 compatibility branch.

See [`CONTRIBUTING.md`](CONTRIBUTING.md) to submit code or reproducible platform artifacts, and [`SECURITY.md`](SECURITY.md) for private vulnerability reporting.

## 许可证 / License

QuizPane 使用 [PolyForm Noncommercial License 1.0.0](LICENSE)：符合许可证定义的个人和非商业使用免费，商业使用需要单独书面授权。商业授权联系 **xutianyoubupt@foxmail.com**，详见 [`COMMERCIAL_LICENSE.md`](COMMERCIAL_LICENSE.md)。

这是一种 **source-available（源码可用）**许可证，不是 OSI 定义的开源许可证。请不要用“MIT”“完全自由使用”描述本项目。

QuizPane is licensed under the [PolyForm Noncommercial License 1.0.0](LICENSE). Qualifying personal and noncommercial use is free; commercial use requires a separate written license. Contact **xutianyoubupt@foxmail.com** and see [`COMMERCIAL_LICENSE.md`](COMMERCIAL_LICENSE.md).

This is a **source-available** license, not an OSI-approved open-source license.

Third-party dependencies retain their own licenses. The QR encoder is based on Project Nayuki's MIT-licensed QR Code generator.
