# QuizPane 架构与 Code Review 导读

本文面向熟悉 Java Web、写过 jQuery 前端，但刚接触 C++/Qt 的开发者。目标不是逐行解释语法，而是帮助你在一次 Code Review 中回答四个问题：

1. 用户点击之后，代码经过了哪些模块？
2. 哪个对象拥有数据和系统资源？
3. Provider 为什么既能支持远程题库，也能支持本地题库？
4. 修改一处代码时，应该运行哪些测试、重点防止什么回归？

> 当前产品名是“题库制作器”，为了避免一次性破坏历史构建路径，源码目录仍保留 `apps/bank-studio`。用户界面、可执行文件和发行包均不再出现 Bank Studio。

## 1. 先建立 Java Web 对照关系

| C++/Qt 模块 | Java Web 类比 | 主要责任 |
|---|---|---|
| `apps/desktop-qt` | Controller + View | 小窗、菜单、登录、答题流程和页面状态 |
| `apps/bank-studio` | 独立 Import 应用 | 文档导入、模型设置、进度展示和异常复核 |
| `core` | Domain + Service | 答题领域对象、草稿持久化、题图隐蔽化 |
| `sdk` | SPI + Adapter + Repository | Provider ABI、动态加载、声明式题库、安装和安全凭据 |
| `providers/demo` | Mock Adapter | 完全离线的原生 Provider 参考实现 |
| `tools/bank-generator` | Batch/Import Tool | 校验 JSON 并确定性生成本地题库包 |
| `tests` | JUnit tests | 领域、安装、Provider 与完整流程回归 |

依赖方向保持单向：

```text
apps/desktop-qt ──> core
        │
        └─────────> sdk ──> miniz / 系统凭据库

apps/bank-studio ──> Qt Widgets / Qt Network

providers/demo ──> provider_abi.h

sdk -X-> 任何具体第三方 Provider
```

这与 Spring 项目中的 Controller → Service → Repository 类似。`sdk` 只认识稳定 ABI，不反向依赖粉笔或其他题库实现，主仓库因此可以独立公开和构建。

## 2. 当前工程目录怎么读

```text
quizpane/
├── apps/
│   ├── desktop-qt/
│   │   ├── src/main.cpp                 # 主程序入口、命令行安装题库
│   │   ├── src/ui/main_window.*         # 小窗页面与流程编排
│   │   ├── src/ui/app_dialogs.*         # 老板键、关于等独立对话框
│   │   ├── src/ui/line_icons.*           # 运行时线性图标
│   │   └── src/platform/                 # 热键、置顶、文件关联
│   └── bank-studio/
│       ├── src/main.cpp                 # 题库制作器入口
│       ├── src/studio_window.*          # 四步向导与本地预检
│       └── src/model_settings_dialog.*  # 厂商目录、模型列表与 API 设置
├── core/
│   ├── attempt.*                        # 标准题目与作答 DTO
│   ├── draft_store.*                    # 原子保存/恢复草稿
│   └── image_privacy_filter.*           # 白底题图透明化
├── sdk/
│   ├── provider_abi.h                   # 原生 Provider 的稳定 C ABI
│   ├── provider_loader.*                # 动态库适配与异步请求
│   ├── declarative_provider.*           # 纯 JSON 本地题库运行时
│   ├── provider_installer.*             # 检查、安装、删除题库包
│   └── zip_archive.*                    # miniz 的窄接口封装
├── providers/demo/                      # 可公开的离线示例
├── schemas/                             # 声明式题库 JSON Schema
├── tools/bank-generator/                # 无 AI 的确定性打包工具
└── tests/                               # CTest 回归测试
```

### 为什么题库制作器拆出 `model_settings_dialog`

以前 `studio_window.cpp` 同时包含四步向导、厂商目录、图标绘制、HTTP 鉴权和模型列表解析，等价于把 Controller、配置类和远程 Client 写进一个 Java 文件。

现在职责为：

- `StudioWindow`：只持有 `ModelSettings` DTO，负责页面和进度；
- `editModelSettings()`：负责对话框事务，取消返回 `std::nullopt`，保存才返回新 DTO；
- `ModelVendor`：仅存在于实现文件，普通向导代码看不到厂商协议细节；
- `QNetworkAccessManager`：生命周期绑定对话框，关闭设置页会自动取消并回收相关对象。

## 3. 六个需要先懂的 C++/Qt 概念

### 3.1 QObject 父子对象树

创建控件时常看到 `new QLabel`，但没有对应 `delete`。控件加入拥有 parent 的布局/窗口后由 Qt 对象树接管，父对象析构时递归释放。

这类似容器托管 Bean，不等于裸指针一定泄漏。普通 C++ 对象若不属于 QObject 树，仍应使用值语义、RAII 或智能指针。

### 3.2 RAII

RAII 类似 Java `try-with-resources`：对象离开作用域即释放文件、动态库或系统句柄。`QSaveFile`、`QLibrary` 和平台热键对象都依赖这一原则。

### 3.3 signal / slot

`connect(button, &QPushButton::clicked, ...)` 类似 jQuery `.on("click", handler)` 或 Spring 事件监听，但信号参数在编译期检查。

如果代码位于 `QObject` 成员函数中，可以直接写 `connect`；普通自由函数必须写 `QObject::connect`。这是把 UI 逻辑抽到独立模块时常见的编译错误。

### 3.4 值语义和 DTO

`Attempt`、`DraftSnapshot`、`ModelSettings` 都是值对象。函数接收 `const T&` 表示只读引用，返回 `std::optional<T>` 表示可能没有结果，对应 Java 的不可变 DTO 和 `Optional<T>`。

### 3.5 C ABI + JSON

Provider 边界不用 C++ 虚函数，而使用 C ABI 函数表和 JSON：

- C ABI 避免不同编译器的 C++ ABI 不兼容；
- JSON 作为稳定 DTO，便于记录、调试和版本演进；
- Host 注入的函数表类似受限依赖注入，只开放日志、网络和凭据能力；
- Provider 无权获得 `MainWindow*`，不能直接操纵主程序 UI。

可以把它理解成同进程内的 HTTP/SPI：导出函数是 endpoint，JSON 是 request/response。

### 3.6 CMake target

每个 `add_library` / `qt_add_executable` 类似 Maven module。依赖必须写在 `target_link_libraries`，头文件路径写在 `target_include_directories`，不要添加全局 include/link 配置。

## 4. 四条核心调用链

### 4.1 题库安装与加载

```text
拖入/选择 .quizpane-provider
  -> MainWindow::installProviderPackage
  -> ProviderInstaller::inspect
       校验 manifest、文件数量、体积、相对路径和符号链接
  -> 用户确认权限
  -> ProviderInstaller::install
       staging 解压 -> 完整校验 -> 原子迁移
  -> MainWindow::loadProvider
  -> ProviderLoader::load 或 DeclarativeProvider
```

Review 重点：

- 任何 ZIP 路径都不能直接拼到目标目录；
- 安装失败不能留下半安装目录；
- 原生动态库与声明式 JSON 必须在统一 Host 行为下工作；
- 删除当前 Provider 时先卸载，再处理 Windows 文件占用导致的延迟删除。

### 4.2 扫码登录

```text
连接题库账号
  -> auth.begin
  -> handleProviderResponse("auth-begin")
  -> 绘制二维码并启动轮询
  -> auth.poll
  -> Host credentialWrite 回调
  -> 系统安全凭据库
  -> catalog.list
```

Cookie/API Token 不应写入普通 JSON、日志或 Git 仓库。系统实现分别是 macOS Keychain、Windows Credential Manager 和 Linux Secret Service。

### 4.3 做题、交卷与解析

```text
catalog.list
  -> populateCatalog
  -> attempt.create
  -> attempt.questions
  -> showQuestion
  -> chooseAnswer（选中后短暂反馈并自动下一题）
  -> attempt.saveAnswer
  -> 自定义交卷气泡
  -> attempt.submit
  -> attempt.report + attempt.solutions
  -> showSolution
```

`request.id` 是回包路由键，作用类似 Promise 标识或 traceId。`handleProviderResponse()` 只根据 ID 分派结果，不把 Provider 特有字段散落到多个页面事件中。

### 4.4 题库制作器

```text
选择/拖入 TXT、Markdown、DOCX、PDF、JSON
  -> StudioWindow::appendSources
  -> 本地预检与 Token 上限估算
  -> 模型设置 editModelSettings
       厂商固定 Endpoint / 自定义 Endpoint
       在线拉取模型列表 / 内置列表回退
  -> 模型整理（待实现）
  -> 规则校验与人工复核（待实现）
  -> 声明式 Provider 打包（待接入现有 bank-generator）
```

当前必须诚实区分：界面、文件导入、本地预检和模型列表获取已经存在；真正的文档解析、模型编排、结构化修复和最终 GUI 打包尚未完成。不要在 Review 或宣传中把占位进度页描述成完整 AI 生成能力。

## 5. Provider 的两种实现

### 原生 Provider

适合远程题库、扫码登录、保存答案和交卷。产物是当前系统/CPU 架构的 `.dylib`、`.dll` 或 `.so`，通过 `provider_abi.h` 导出六个稳定符号。

### 声明式 Provider

适合用户自备题库。内容是 `manifest.json + content/bank.json + assets/`，最终 ZIP 为 `.quizpane-provider`。无需编译器，Windows/macOS/Linux 可以使用同一个包。

两者在安装后都通过统一请求方法向 `MainWindow` 提供分类、题目、答案和解析，所以本地题库不必模拟某一家远程题库的“套题接口”。声明式运行时负责把平铺题目按分类和题量组装成一次练习。

## 6. 推荐阅读顺序

第一次 Review 不要从 1600 行的 `main_window.cpp` 第一行硬啃到底，按下面顺序更容易形成闭环：

1. `core/include/quizpane/attempt.hpp`：标准题目和练习 DTO；
2. `tests/attempt_roundtrip_test.cpp`：DTO 如何序列化；
3. `providers/demo/demo_provider.cpp`：一个完整但无网络的 RPC 示例；
4. `sdk/include/quizpane/provider_abi.h`：Host 与动态库的边界；
5. `sdk/src/provider_loader.cpp`：加载、请求回调和凭据注入；
6. `sdk/src/provider_installer.cpp`：题库包的安全安装事务；
7. `apps/desktop-qt/src/ui/main_window.hpp`：页面状态和成员所有权；
8. 按 `main_window.cpp` 中的章节标题阅读登录、答题、安装和窗口四组方法；
9. `apps/bank-studio/src/model_settings_dialog.*`：独立设置 DTO 与网络适配；
10. `apps/bank-studio/src/studio_window.*`：四步向导；
11. `tests/`：用例所保证的系统行为。

## 7. 按文件 Review 时看什么

### `main_window.hpp/.cpp`

- 新状态是否真的属于窗口会话，还是应该放进 Core/SDK；
- 异步回包是否通过唯一 ID 路由；
- 页面切换后旧定时器是否可能继续修改新页面；
- Qt 裸指针是否由父对象拥有；
- UI 文案是否泄漏 `Provider`、ABI 等技术概念给普通用户；
- 登录页是否保持 compact，答题页滚动是否只影响正文。

### `provider_loader.cpp`

- 导出符号、ABI 版本和 descriptor 是否全部校验；
- 回调是否通过 queued connection 回到 Qt 主线程；
- 动态库卸载后是否仍可能留下指向库代码的回调；
- 凭据 key 是否按 providerId 隔离；
- 日志是否可能包含 Cookie、Token、二维码会话或原始题目。

### `provider_installer.cpp`

- 是否防止 `../`、绝对路径、符号链接和 zip-slip；
- 是否限制文件数量与解压总大小；
- inspect 后文件被替换时，install 是否重新验证；
- staging 失败是否清理；
- Windows 文件占用是否得到可理解的用户提示。

### `model_settings_dialog.cpp`

- 固定厂商是否禁止编辑 Endpoint；
- 自定义供应商 URL 是否进行基本合法性检查；
- API Key 是否只作为请求头，且没有进入日志/普通配置；
- Anthropic 与 OpenAI 兼容鉴权是否明确分支；
- 请求失败是否回退到推荐模型，而不是把对话框卡死；
- 对话框关闭后异步回调是否仍可能访问已销毁对象。

### `core`

- 领域逻辑是否仍与 UI/网络解耦；
- 草稿是否使用原子写入；
- 图片算法是否保持单次像素扫描，避免引入 OpenCV 等重依赖；
- 新字段序列化时是否考虑旧草稿缺失字段。

## 8. 代码规范

仓库根目录提供 `.clang-format`。基本约束：

- C++20，4 空格缩进，不使用 Tab；
- 目标代码尽量控制在 100 列；
- 单行 `if/for` 在影响可读性时展开；
- 头文件只暴露调用方需要的类型，具体厂商表等实现细节留在 `.cpp`；
- 优先使用值语义、`const`、`std::optional` 和 RAII；
- 注释解释“为什么”和边界条件，不把代码逐字翻译成中文；
- 用户可见文案使用“题库”“题库制作器”，内部技术文档才使用 Provider/ABI；
- 不提交 API Key、Cookie、抓包、私有协议、真实题库内容和构建产物。

安装 `clang-format` 后可检查近期修改：

```bash
clang-format --dry-run --Werror \
  apps/bank-studio/src/*.cpp \
  apps/bank-studio/src/*.hpp
```

## 9. 构建与验证

macOS 开发环境：

```bash
brew install cmake ninja qtbase
export CMAKE_PREFIX_PATH="$(brew --prefix qtbase)"
cmake --preset dev
cmake --build --preset dev --parallel
ctest --preset dev
```

如果只改文档，也至少运行：

```bash
git diff --check
```

如果改 Provider 安装、草稿、答题或声明式题库，应运行完整 7 项 CTest。修改平台窗口/热键代码还必须在对应系统真机验证，因为 macOS AppKit、Windows Win32 与 Linux X11 的行为不能靠一台机器完全覆盖。

## 10. 提交前检查单

- [ ] `git status` 中没有 API Key、抓包、真实题库或构建目录；
- [ ] 新类/函数的所有权和线程边界清楚；
- [ ] 用户取消对话框不会留下半更新状态；
- [ ] 网络失败有可理解的回退或提示；
- [ ] 原生 Provider 与声明式 Provider 的公共流程没有分叉失控；
- [ ] `cmake --build` 成功；
- [ ] `ctest` 全部通过；
- [ ] `git diff --check` 无空白错误；
- [ ] README、TODO 和实际完成度一致；
- [ ] 第三方题库与协议遵守 [`legal/THIRD_PARTY_CONTENT_POLICY.md`](../legal/THIRD_PARTY_CONTENT_POLICY.md)。

## 11. 后续最值得继续拆分的地方

`main_window.cpp` 仍然较长，但当前已经通过章节标题把职责分组。下一阶段若继续增长，建议按成员函数实现文件拆成：

- `main_window_session.cpp`：登录、目录、答题、交卷、解析；
- `main_window_providers.cpp`：安装、加载、切换、删除；
- `main_window_shell.cpp`：托盘、老板键、PIN、尺寸和平台事件。

C++ 允许同一个类的成员函数分布在多个 `.cpp`，不需要改成继承层次。拆分前应先为关键 UI 流程补足自动化测试，避免只为了缩短文件而引入难以发现的交互回归。
