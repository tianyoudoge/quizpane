# QuizPane 项目导读、架构与重构建议

> 本目录（`spec/refactor/`）面向**首次接触本项目、有服务端（Java/Spring）背景的工程师**，
> 提供一份可快速上手的项目导读、架构与数据流图、以及分模块的重构 / 性能 / 可读性优化建议。
>
> 与 `spec/` 其它文档的关系：`spec/01~06` 是**产品关键逻辑的权威参考**（改代码前必读）；
> 本目录是**架构视角的地图和优化清单**，不重复 01~06 已讲透的规则识别细节，而是补齐
> 「做题 / 接口题库 / 小窗视频」三块的结构说明，并把五块统一到一张架构图上。
>
> 建立于 2026-09-05，对照分支 `refactor/architecture-review`。代码位置用稳定的类名/函数名
> 表达，行号可能漂移。**代码与本文冲突时以代码和测试为准。**

## 阅读顺序

1. 先看本页「一、给服务端工程师的心智映射」——把 Qt/C++ 桌面项目翻译成你熟悉的服务端概念。
2. 再看「二、五大子系统导读」的索引，按需跳转到分册。
3. 想改代码时，回到 `spec/01-总览.md` 和对应主题文档核对产品约束。

## 分册索引

| 文档 | 覆盖子系统 | 关注点 |
|---|---|---|
| [01-用户做题逻辑.md](01-用户做题逻辑.md) | MainWindow 做题主流程 | 页面状态机、RPC 时序、草稿、判分 |
| [02-题库创建与识别.md](02-题库创建与识别.md) | 规则链路 + MinerU 链路 | 两条提取链路的分工、复核、打包 |
| [03-接口类题库实现.md](03-接口类题库实现.md) | C ABI 插件框架 | Provider ABI、ProviderLoader、声明式题库 |
| [04-小窗视频实现.md](04-小窗视频实现.md) | 外部窗口镜像/置顶 | macOS 捕获 vs Windows 置顶、浏览器桥 |
| [05-重构与优化建议汇总.md](05-重构与优化建议汇总.md) | 全项目 | 跨模块优先级、可读性、性能清单 |

每个分册结构统一：**功能定位 → 架构/数据流图 → 关键代码导读 → 重构建议 → 性能建议 → 可读性建议**。

## 架构图与数据流图（SVG）

全部位于 [`assets/`](assets/)，可直接在浏览器或支持 SVG 的 Markdown 预览中打开。

### 全局架构

| 图 | 说明 |
|---|---|
| [architecture-simple.svg](assets/architecture-simple.svg) | **简单版**：五层分层视图（外壳/展示/服务/领域/数据），对标服务端分层 |
| [architecture-complex.svg](assets/architecture-complex.svg) | **复杂版**：两个可执行程序 + 共享库的组件依赖全景 |

### 全局数据流

| 图 | 说明 |
|---|---|
| [dataflow-overview-simple.svg](assets/dataflow-overview-simple.svg) | **简单版**：从原始资料到刷题的一条主链路 |

### 分子系统数据流（各有简单/复杂两版）

| 子系统 | 简单版 | 复杂版 |
|---|---|---|
| 用户做题 | [dataflow-quiz-simple.svg](assets/dataflow-quiz-simple.svg) | [dataflow-quiz-complex.svg](assets/dataflow-quiz-complex.svg) |
| 题库创建与识别 | [dataflow-bank-simple.svg](assets/dataflow-bank-simple.svg) | [dataflow-bank-complex.svg](assets/dataflow-bank-complex.svg) |
| 接口类题库 | [dataflow-provider-simple.svg](assets/dataflow-provider-simple.svg) | [dataflow-provider-complex.svg](assets/dataflow-provider-complex.svg) |
| 小窗视频 | [dataflow-video-simple.svg](assets/dataflow-video-simple.svg) | [dataflow-video-complex.svg](assets/dataflow-video-complex.svg) |

---

## 一、给服务端工程师的心智映射

如果你熟悉 Java Web / Spring，下面这张对照表能帮你把本项目"翻译"成熟悉的概念。
**注意：这只是帮助理解的类比，写进代码的注释里请勿使用（`CONTRIBUTING.md` 明确禁止跨技术栈类比）。**

| 服务端概念 | QuizPane 对应 | 位置 |
|---|---|---|
| 多模块 Maven 项目（父 pom + 子模块） | 顶层 `CMakeLists.txt` + 各目录子 `CMakeLists.txt` | 根目录、`core/`、`sdk/`、`apps/` |
| `mvn compile / test` | `cmake --build --preset dev` / `ctest --preset dev` | `CONTRIBUTING.md` |
| 启动类模块（`@SpringBootApplication`） | 两个可执行程序 `apps/desktop-qt`、`apps/bank-studio` | `apps/` |
| Controller（编排、不放业务） | `MainWindow` / `StudioWindow`（UI 控制器） | `apps/*/src/ui/` |
| Service（业务逻辑） | `core/`、`bank-studio/engine/`（纯逻辑，无 UI 依赖） | `core/src/`、`engine/src/` |
| Repository / DAO | `DraftStore`、`bank.json` 读写、`QSettings` | `core/src/draft_store.cpp` 等 |
| Entity / DTO | `Attempt`、`ExtractedDocument`、`ReviewResult`（struct） | `*/include/quizpane/*.hpp` |
| 进程内 HTTP/RPC + DTO | 题库 **C ABI + JSON-RPC**（函数指针=endpoint，JSON=DTO） | `sdk/include/quizpane/provider_abi.h` |
| 依赖注入（`@Autowired`） | `qp_host_api_v1` 能力表由 Host 注入给 Provider | `provider_abi.h:46` |
| Bean 生命周期 | `qp_provider_create` / `destroy`；Qt 父子对象树管理控件所有权 | — |
| 事件总线 / 观察者 | Qt **signal/slot**（`connect(...)`），跨线程默认排队到主线程 | `provider_loader.hpp:31` |
| `@Transactional` 提交后可见 | `QSaveFile` 先写临时文件、commit 才替换（草稿原子写） | `draft_store.cpp:60` |
| 异步任务 / 线程池 | `QtConcurrent` 工作线程（题库生成） | `generation_workflow.cpp` |
| JSON Schema / OpenAPI 契约 | `schemas/*.json`（**滞后**）+ C++ 校验器（**权威**） | `core/src/bank_validator.cpp` |
| 配置中心 | `QSettings`（跨平台键值，Windows 落注册表） | 全项目 |

### 你最需要先建立的三个"不一样"

1. **头文件 `.hpp` 与实现 `.cpp` 分离**：接口声明在 `include/quizpane/*.hpp`，实现在 `src/*.cpp`。
   改方法签名要同时改两处。这是 C++ 与 Java 单文件最大的物理差异。

2. **所有权与 RAII，而非 GC**：没有垃圾回收。对象要么是栈上的值成员（析构自动清理，
   顺序与声明相反，见 `main_window.hpp:136`），要么由 Qt 父子对象树持有（parent 析构时
   自动删子控件，所以成员里那一堆 `QLabel*` 是**非拥有引用**）。`std::unique_ptr` 用于
   独占持有平台后端（`external_window_manager.cpp:39`）。

3. **UI 单线程 + 信号槽异步**：所有 UI 操作必须在主线程；耗时工作丢给 `QtConcurrent`，
   结果通过 signal 排队回主线程。题库 RPC 的回包甚至可能来自动态库的网络线程，靠
   `ProviderLoader::generation_` 代号防止旧回包错误路由（`provider_loader.hpp:66`）。

## 二、五大子系统导读（概览）

```
                    ┌────────────────── apps/bank-studio（制作端）──────────────────┐
  原始资料 ─────────▶│  ② 题库创建与识别：本地/MinerU 提取 → 规则引擎 → 复核 → 打包    │
  (txt/docx/pdf)     └───────────────────────────────┬───────────────────────────────┘
                                                      │ .quizpane-provider 包 + handoff
                                                      ▼
                    ┌────────────────── apps/desktop-qt（刷题端）──────────────────┐
                    │  ③ 接口类题库：ProviderLoader ⇄ C ABI ⇄ DeclarativeProvider   │
                    │  ① 用户做题：MainWindow 页面状态机（目录→做题→交卷→解析）     │
                    │  ④ 小窗视频：BrowserBridge ⇄ ExternalWindowManager ⇄ 平台后端  │
                    └───────────────────────────────────────────────────────────────┘
```

- **① 用户做题**（[分册](01-用户做题逻辑.md)）：`MainWindow` 是 2987 行的页面状态机，
  通过 `ProviderLoader` 向题库发 9 个 JSON-RPC 方法完成"目录→组卷→作答→交卷→解析"。
- **② 题库创建与识别**（[分册](02-题库创建与识别.md)）：两条链路（本地提取 / MinerU 云提取）
  汇聚到统一的 `ExtractedDocument`，再进同一个确定性规则引擎，产出 `ReviewResult`，
  复核后打包成 `.quizpane-provider`。细节以 `spec/02~05` 为权威。
- **③ 接口类题库**（[分册](03-接口类题库实现.md)）：`provider_abi.h` 定义 C ABI 插件契约；
  内置 `DeclarativeProvider` 用 `bank.json` 直接实现该契约，第三方可编译独立 `.dll/.so`。
- **④ 小窗视频**（[分册](04-小窗视频实现.md)）：`ExternalWindowManager` 统一入口，
  macOS 用 ScreenCaptureKit 镜像画面（可交互、需录屏权限），Windows 用 `SetWindowPos`
  直接置顶原生窗口（不镜像、无需权限）。

重构与优化的**跨模块优先级清单**见 [05-重构与优化建议汇总.md](05-重构与优化建议汇总.md)。
