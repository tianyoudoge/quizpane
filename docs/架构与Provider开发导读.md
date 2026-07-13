# QuizPane 架构与 Provider 开发导读

本文面向熟悉 Java Web、但刚接触 C++/Qt 的开发者。

## 模块映射

| C++ 模块 | Java Web 类比 | 责任 |
|---|---|---|
| `apps/desktop-qt` | Controller + View | 窗口、菜单、用户交互和流程编排 |
| `core` | Domain/Service | 答题状态、草稿和图片处理 |
| `sdk` | SPI + Adapter | Provider ABI、动态加载、安装和安全存储 |
| `providers/demo` | Mock Adapter | 完全离线的参考实现 |
| `tools/bank-generator` | Import Tool | 本地题库源校验与后续打包 |
| `tests` | JUnit tests | Host、领域模型和 Provider 流程测试 |

依赖方向：

```text
desktop-qt -> core
desktop-qt -> sdk
demo-provider -> provider_abi.h
sdk -X-> 任何具体第三方 Provider
```

## C++/Qt 心智模型

- `QObject` 父子关系类似容器托管的 Bean 生命周期：父对象析构时自动释放子对象；
- RAII 类似 Java 的 `try-with-resources`，对象离开作用域即释放文件、锁或动态库句柄；
- signal/slot 类似带类型检查的事件总线或 jQuery `.on()`；
- `std::unique_ptr` 表示唯一所有权，`std::optional` 表示可能不存在的结果；
- C ABI + JSON 类似进程内的 HTTP/SPI：导出函数是 endpoint，JSON 是 DTO；
- CMake target 类似 Maven module，依赖通过 `target_link_libraries` 显式声明。

## 推荐阅读顺序

1. `core/include/quizpane/attempt.hpp`：领域模型；
2. `providers/demo/demo_provider.cpp`：完整 RPC 示例；
3. `sdk/include/quizpane/provider_abi.h`：跨编译器 ABI；
4. `sdk/src/provider_loader.cpp`：动态加载、异步回调和安全凭据；
5. `sdk/src/provider_installer.cpp`：安装检查与路径穿越防护；
6. `apps/desktop-qt/src/ui/main_window.cpp`：UI 状态机与流程编排；
7. `tests/`：用例描述的系统行为。

## Provider 最小职责

Provider 通过 descriptor 声明 ID、名称、版本和能力，并实现初始化、分类、创建练习、题目、保存答案、交卷、报告和解析等适用方法。不支持的能力必须明确声明，Host 依据能力调整界面。

Provider 是本地原生代码。开发与分发时必须遵守 [`legal/THIRD_PARTY_CONTENT_POLICY.md`](../legal/THIRD_PARTY_CONTENT_POLICY.md)，不得把真实凭据、私有协议或无授权内容提交到公共仓库。
