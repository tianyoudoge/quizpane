# C++ 从 0 到 1 学习导读：基于 QuizPane 项目

这份文档不是 C++ 语法大全，而是给已经有 Java Web 服务端经验、但 C++ 工程经验较少的人准备的项目导读。目标是让你能从基本语法过渡到读懂并修改 QuizPane 这种真实桌面应用：知道代码在哪里、模块怎么连、对象怎么传、怎么构建、怎么测试，以及 C++ 和 Java Web 的思维差异在哪里。

读完后的最低目标：

- 能看懂 `core`、`sdk`、`apps/bank-studio/engine` 这三层后端处理代码；
- 能理解 Qt 的 `QString`、`QVector`、`QJsonObject`、信号槽和事件循环；
- 能用 CMake 构建项目、跑测试、定位一个失败用例；
- 能完成一个小需求，例如给题库 JSON 增加字段校验，或给题库制作流程增加一条状态规则。

## 1. 先建立 C++ 项目的整体地图

QuizPane 不是单个 `main.cpp` 程序，而是一个多模块 C++/Qt 工程。可以先用 Java Web 的分层经验理解它：

| Java Web 常见概念 | QuizPane 对应位置 | 作用 |
|---|---|---|
| domain/model | `core/include/quizpane/*.hpp` | 领域对象、草稿、答题、题库校验 |
| service | `apps/bank-studio/engine/src/*.cpp` | 题库制作器的业务流程 |
| repository | `CheckpointStore`、`DraftStore` | 本地文件持久化 |
| gateway/client | `ModelClient`、`ProviderLoader` | 调模型、加载题库 Provider |
| DTO/schema | `QJsonObject`、`schemas/*.json` | 跨模块传输的数据结构 |
| controller/UI adapter | `apps/*/src/*.cpp` | Qt 窗口层，接收用户动作并调用业务流程 |
| integration test | `tests/*.cpp` | 可执行测试，覆盖真实流程 |

项目目录可以按这个顺序看：

```text
core/                       纯业务基础层：答题、草稿、题库 JSON 校验
sdk/                        题库 Provider 运行时和安装逻辑
apps/desktop-qt/            小窗刷题主程序
apps/bank-studio/engine/    题库制作器后端流程
apps/bank-studio/src/       题库制作器 Qt 界面
providers/demo/             原生 Demo Provider
schemas/                    声明式题库 JSON 示例与 schema
tests/                      C++ 可执行测试
tools/bank-generator/       CLI 工具
```

如果你主要关心后端处理流程，优先看这几条链路：

```text
题库 JSON 校验：
schemas/examples/.../bank.json
  -> core/src/bank_validator.cpp
  -> tests/bank_validator_test.cpp

答题草稿：
core/include/quizpane/attempt.hpp
  -> core/src/attempt.cpp
  -> core/src/draft_store.cpp
  -> tests/draft_store_test.cpp

题库制作：
apps/bank-studio/engine/include/quizpane/studio/generation_workflow.hpp
  -> apps/bank-studio/engine/src/generation_workflow.cpp
  -> apps/bank-studio/engine/src/chunker.cpp
  -> apps/bank-studio/engine/src/checkpoint_store.cpp
  -> tests/generation_workflow_test.cpp
```

## 2. C++ 和 Java 最大的差异

你从 Java 转 C++，最需要先改的是对象生命周期和依赖边界的心智模型。

Java 里对象通常交给 GC，方法参数大多是引用语义。C++ 里每一行都隐含“这个对象归谁管、何时析构、拷贝还是引用”的问题。

### 2.1 值、引用、指针

看这个项目里的声明：

```cpp
void start(const QStringList& sourcePaths, const ModelSettings& settings,
           const QString& resumeTaskId = {});
```

你可以这样理解：

- `QStringList` 是 Qt 的字符串列表，类似 `List<String>`；
- `const QStringList&` 表示只读引用，不复制整个列表；
- `ModelSettings` 是配置对象，只读引用传入；
- `resumeTaskId = {}` 表示默认参数，调用方可以不传。

常见参数写法：

```cpp
void f(QString value);          // 传值：会复制或移动，适合小对象或需要持有副本
void f(const QString& value);   // 只读引用：不复制，适合读取输入
void f(QString& value);         // 可写引用：调用方对象会被修改
void f(QString* value);         // 指针：可能为空，也常用于可选输出参数
```

项目里经常出现这种可选错误输出：

```cpp
bool load(const QString& bankPath, QString* error = nullptr);
```

这相当于 Java 里返回 `boolean`，同时通过一个可选 holder 带出错误信息。C++ 不强制你用异常，很多底层和 Qt 代码会用这种风格。

### 2.2 头文件和源文件

C++ 通常拆成 `.hpp` 和 `.cpp`：

- `.hpp` 放类型声明、函数签名、类的公开接口；
- `.cpp` 放具体实现；
- 编译器按翻译单元分别编译，再由链接器合并。

例如：

```text
core/include/quizpane/attempt.hpp  声明 Attempt、AnswerDraft
core/src/attempt.cpp               实现 Attempt::toJson/fromJson
```

读代码时先看 `.hpp`，相当于先看 Java 的 public API；再看 `.cpp`，相当于看实现细节。

### 2.3 namespace

项目里的业务代码基本放在 `quizpane` 或 `quizpane::studio` 命名空间下：

```cpp
namespace quizpane {
// ...
}

namespace quizpane::studio {
// ...
}
```

可以类比 Java package。区别是 C++ namespace 只影响符号名称，不要求目录必须匹配，但本项目基本保持了目录和命名空间一致。

### 2.4 RAII

RAII 是 C++ 工程里最重要的思想：资源跟着对象生命周期走。对象构造时拿资源，析构时释放资源。

Java 常见写法是：

```java
try (var input = new FileInputStream(path)) {
    // use input
}
```

C++ 更常见是局部对象离开作用域自动析构。Qt 里也有类似对象，例如 `QSaveFile` 用于原子写文件：对象负责打开、写入、提交，失败时不会留下半截文件。

在项目里读 `checkpoint_store.cpp` 时，要重点看这种“对象生命周期就是事务边界”的写法。

## 3. 必须先掌握的 std 和 Qt 容器

这个项目用 Qt 较多，所以你会看到 `QString`、`QVector`、`QHash`、`QSet` 多于 `std::string`、`std::vector`。

### 3.1 Qt 类型和 std 类型对照

| Qt 类型 | std/Java 近似概念 | 项目用途 |
|---|---|---|
| `QString` | `std::string` / `String` | 文本 |
| `QStringList` | `std::vector<std::string>` / `List<String>` | 字符串列表 |
| `QVector<T>` | `std::vector<T>` / `ArrayList<T>` | 连续数组 |
| `QList<T>` | `List<T>` | 通用列表 |
| `QHash<K,V>` | `unordered_map` / `HashMap` | 哈希表 |
| `QSet<T>` | `unordered_set` / `HashSet` | 去重集合 |
| `QJsonObject` | `Map<String,Object>` | JSON 对象 |
| `QJsonArray` | `List<Object>` | JSON 数组 |
| `QJsonValue` | `Object` | JSON 任意值 |

### 3.2 范围 for

项目里大量使用：

```cpp
for (const auto& questionId : questionIds) {
    questionIdsJson.append(questionId);
}
```

这和 Java 的 enhanced for 类似：

```java
for (String questionId : questionIds) {
    questionIdsJson.add(questionId);
}
```

几个细节：

- `auto` 让编译器推断类型；
- `const auto&` 表示只读引用，避免复制；
- 如果循环里要修改元素，才用 `auto&`。

### 3.3 JSON 处理

看 `core/src/attempt.cpp`：

```cpp
QJsonObject Attempt::toJson() const {
    QJsonArray questionIdsJson;
    for (const auto& questionId : questionIds) {
        questionIdsJson.append(questionId);
    }
    return {{"id", id}, {"questionIds", questionIdsJson}};
}
```

这里的返回值初始化语法比较像 JavaScript 对象字面量，但它是 C++ initializer list。`toJson()` 后面的 `const` 表示这个成员函数不修改当前对象。

反序列化时：

```cpp
Attempt Attempt::fromJson(const QJsonObject& json) {
    Attempt result;
    result.id = json.value("id").toString();
    return result;
}
```

这是典型的“创建结果对象 -> 从 JSON 填字段 -> 返回值”。现代 C++ 会做返回值优化，不需要像早期 C++ 那样担心这里必然昂贵。

## 4. CMake：C++ 项目的 Maven/Gradle

QuizPane 用 CMake 管理构建。根目录 `CMakeLists.txt` 类似父 POM：

```cmake
project(QuizPane VERSION 0.2.4 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets Network)
add_subdirectory(core)
add_subdirectory(sdk)
add_subdirectory(apps/desktop-qt)
add_subdirectory(apps/bank-studio)
add_subdirectory(tests)
```

每个子目录有自己的 target。target 可以理解成 Maven module 或 Gradle subproject 的产物：

```cmake
add_library(quizpane_core STATIC
    src/draft_store.cpp
    src/image_privacy_filter.cpp
    src/attempt.cpp
    src/bank_validator.cpp
)

target_link_libraries(quizpane_core PUBLIC Qt6::Core Qt6::Gui)
```

这段表示：

- 构建一个静态库 `quizpane_core`；
- 它由这些 `.cpp` 编译而来；
- 它依赖 Qt Core 和 Qt Gui；
- 其他 target 链接它时也能获得 `PUBLIC` 依赖。

常用命令：

```bash
cmake --preset dev
cmake --build build/dev -j4
ctest --test-dir build/dev --output-on-failure
```

你可以把它们分别类比成：

```text
生成构建目录     mvn generate-sources / gradle configure
编译             mvn package -DskipTests
跑测试           mvn test
```

## 5. 从 core 层开始读

`core` 是最适合作为入门的部分，因为它最接近后端业务代码，几乎不涉及 UI。

### 5.1 Attempt：领域对象和 JSON 序列化

入口文件：

```text
core/include/quizpane/attempt.hpp
core/src/attempt.cpp
tests/attempt_roundtrip_test.cpp
```

先看结构体：

```cpp
struct Attempt {
    QString id;
    QString providerId;
    QString remoteId;
    QString catalogNodeId;
    QStringList questionIds;
    QVector<AnswerDraft> answers;
    AttemptManagement management = AttemptManagement::HostManaged;
    AttemptState state = AttemptState::Preparing;

    [[nodiscard]] QJsonObject toJson() const;
    static Attempt fromJson(const QJsonObject& json);
};
```

学习点：

- `struct` 默认 public，`class` 默认 private；
- `enum class` 是强类型枚举，避免普通 enum 污染命名空间；
- 字段可以在声明处给默认值；
- `static` 成员函数不依赖对象实例；
- `[[nodiscard]]` 提醒调用方不要忽略返回值。

建议练习：

1. 给 `Attempt` 增加一个 `createdAt` 字符串字段；
2. 修改 `toJson()` 和 `fromJson()`；
3. 修改 `tests/attempt_roundtrip_test.cpp`；
4. 跑单测确认序列化往返没有丢字段。

### 5.2 BankValidator：规则校验

入口文件：

```text
core/include/quizpane/bank_validator.hpp
core/src/bank_validator.cpp
tests/bank_validator_test.cpp
```

这部分可以按 Java 后端的 validator/service 看。核心函数是：

```cpp
QList<BankValidationError> validateBankDetailed(const QJsonObject& bank);
```

它接收题库 JSON，返回错误列表。没有抛异常，也没有直接弹 UI，是很干净的领域校验边界。

读这类代码时先找三个东西：

- 输入：`QJsonObject bank`
- 输出：`QList<BankValidationError>`
- 状态：局部 `QSet`、`QHash` 用于去重和交叉引用校验

里面有几个常见 C++/Qt 模式：

```cpp
static const QSet<QString> bankKeys{
    "schemaVersion", "title", "description", "catalogs", "materials", "questions"
};
```

`static const` 局部变量只初始化一次，适合存规则表。

```cpp
if (!bank.value("catalogs").isArray()) {
    errors.append({-1, {}, QStringLiteral("catalogs 必须是数组"), {}});
}
```

`QStringLiteral` 是 Qt 的字符串字面量优化。中文提示文本在 Qt 项目里用它很常见。

建议练习：

1. 找到 `validId()`，理解 ID 正则；
2. 找到 `validateMaterials()`，理解材料和题目的关联校验；
3. 添加一条规则，例如 `bank.description` 不能为空；
4. 在 `bank_validator_test.cpp` 里加失败用例。

## 6. 再看 sdk：接口边界和运行时

`sdk` 是 QuizPane 主程序和题库包之间的边界。

重点文件：

```text
sdk/include/quizpane/declarative_provider.hpp
sdk/src/declarative_provider.cpp
sdk/include/quizpane/provider_installer.hpp
sdk/src/provider_installer.cpp
sdk/src/zip_archive.cpp
```

### 6.1 DeclarativeProvider

头文件：

```cpp
class DeclarativeProvider final {
public:
    bool load(const QString& bankPath, QString* error = nullptr);
    [[nodiscard]] QJsonObject descriptor() const;
    [[nodiscard]] QJsonObject request(const QJsonObject& request);
    [[nodiscard]] bool isLoaded() const { return !providerId_.isEmpty(); }
    void unload();

private:
    QString providerId_, providerName_, providerVersion_;
    QJsonArray catalogs_, questions_, materials_, activeQuestions_;
    QHash<QString, QJsonObject> materialsById_;
};
```

学习点：

- `final` 表示这个类不允许再被继承；
- public 区域是外部 API，private 区域是内部状态；
- 成员变量常用尾部 `_` 区分；
- 小函数可以直接写在头文件里，例如 `isLoaded()`；
- `request(QJsonObject)` 像一个本地 RPC 入口。

这里的设计和 Web Controller 不完全一样。它不是监听 HTTP，而是在进程内接收 JSON 请求。但边界思想一样：外部只知道 `load`、`descriptor`、`request`，不知道内部怎么组织题目和材料。

### 6.2 ProviderInstaller

这个模块负责安装 `.quizpane-provider` 包。你可以把它理解成“上传 ZIP 包 -> 校验 manifest -> 展开到本地目录 -> 注册可用题库”的后端服务。

读这类文件时，不要陷入每个文件 API，而是先画出流程：

```text
输入 provider 包
  -> 解压或读取 manifest
  -> 校验 schemaVersion、providerId、权限
  -> 写入本地安装目录
  -> 返回安装结果或错误
```

这是 C++ 项目里很典型的处理链：尽量用小函数把 IO、校验、状态更新分开。

## 7. 题库制作器 engine：最像后端业务流程的部分

`apps/bank-studio/engine` 是最值得你深入读的模块。它不关心怎么画按钮，主要负责“文档 -> 切块 -> 调模型 -> 校验修复 -> 检查点 -> 候选题库”的业务流程。

核心文件：

```text
apps/bank-studio/engine/include/quizpane/studio/chunker.hpp
apps/bank-studio/engine/src/chunker.cpp
apps/bank-studio/engine/include/quizpane/studio/generation_workflow.hpp
apps/bank-studio/engine/src/generation_workflow.cpp
apps/bank-studio/engine/include/quizpane/studio/checkpoint_store.hpp
apps/bank-studio/engine/src/checkpoint_store.cpp
apps/bank-studio/engine/include/quizpane/studio/model_client.hpp
apps/bank-studio/engine/src/model_client.cpp
```

### 7.1 Chunker：输入预处理

`Chunker` 做的事类似后端批处理里的 splitter：

```text
原始文档文本
  -> SourceBlock：识别材料、题组、普通段落
  -> TextChunk：按 token 预算打包请求
```

为什么不直接按固定字符数切？因为材料题有共享材料和子题，如果切断，模型就会丢上下文。所以这里引入了 `SourceBlock` 和“不可拆块”的概念。

你读 `chunker.hpp` 时重点看数据结构，不必一开始钻正则细节：

```cpp
struct SourceBlock {
    int index = 0;
    QString source;
    QString text;
    int estimatedTokens = 0;
    bool indivisible = false;
};

struct TextChunk {
    QString text;
    int estimatedTokens = 0;
    QVector<int> sourceBlockIndices;
};
```

这相当于 Java 里的两个 DTO。`SourceBlock` 是语义块，`TextChunk` 是模型请求块。一个 chunk 可以包含多个 source block。

### 7.2 GenerationWorkflow：应用服务

`GenerationWorkflow` 可以类比 Spring 里的 application service。它组合了：

- `DocumentExtractor`：提取文档文本；
- `Chunker`：切块；
- `ModelClient`：调用模型；
- `BankValidator`：校验模型输出；
- `CheckpointStore`：保存进度；
- Qt signal：向 UI 层通知进度和结果。

头文件里的状态枚举很关键：

```cpp
enum class WorkflowStage {
    Idle,
    Extracting,
    Chunking,
    Generating,
    Validating,
    Repairing,
    Packaging,
    Done,
    Failed
};
```

这就是业务状态机。读 `.cpp` 时建议按状态迁移读，不要按函数顺序硬读：

```text
start()
  -> 提取文档
  -> 切块
  -> processNextChunk()
  -> requestChunk(false)
  -> handleModelResult()
  -> validate
  -> 必要时 requestChunk(true)
  -> completeChunk()
  -> 保存 checkpoint
  -> processNextChunk()
  -> finished()
```

这条链路和 Java 后端的异步任务非常像，只是它跑在 Qt 事件循环里，结果通过 signal 回调，而不是通过 HTTP response 返回。

### 7.3 CheckpointStore：本地持久化

检查点的目标是任务中断后能恢复。你可以把它理解成轻量版 job repository。

它保存的不是 UI 状态，而是业务恢复所需的最小信息：

- taskId；
- source 文件摘要；
- 已完成 SourceBlock；
- 已生成材料和题目；
- token 统计；
- 需要人工复核的问题。

读这个模块时重点看：

- 如何判断源文件是否还是同一批；
- 如何拒绝过期结构；
- 如何保证写文件原子性；
- 哪些字段属于业务状态，哪些只是展示状态。

## 8. Qt 基础：信号槽、QObject、事件循环

如果只写 Java Web，你习惯的是请求线程进来、处理、返回。Qt 桌面程序是事件驱动：

```text
用户点击按钮 / 网络请求完成 / 定时器触发
  -> Qt 事件循环分发事件
  -> QObject slot 或 lambda 被调用
  -> 更新状态或发出 signal
```

`GenerationWorkflow` 继承 `QObject`：

```cpp
class GenerationWorkflow final : public QObject {
    Q_OBJECT
public:
    explicit GenerationWorkflow(QNetworkAccessManager* manager, QObject* parent = nullptr);

signals:
    void progressChanged(const WorkflowProgress& progress);
    void questionsReady(const GeneratedBankCandidate& candidate);
    void failed(const QString& error);
    void finished();
};
```

要点：

- `Q_OBJECT` 启用 Qt 元对象能力，信号槽依赖它；
- `signals:` 下声明的是事件通知；
- 调用方通过 `connect()` 订阅；
- 网络请求完成后不会阻塞等待，而是回到事件循环里处理。

这和 Java 里的 listener、callback、event bus 有相似之处，但 Qt 把它做成了语言旁路的元对象系统，需要 CMake 的 `CMAKE_AUTOMOC` 参与构建。

## 9. 测试：先从可执行测试理解项目

C++ 项目不一定使用 JUnit 风格框架。这个项目很多测试就是普通可执行文件：

```cpp
int main() {
    quizpane::Attempt attempt;
    attempt.id = "attempt-1";
    const auto restored = quizpane::Attempt::fromJson(attempt.toJson());
    return restored.id == attempt.id ? EXIT_SUCCESS : EXIT_FAILURE;
}
```

CMake 里这样注册：

```cmake
add_executable(attempt_roundtrip_test attempt_roundtrip_test.cpp)
target_link_libraries(attempt_roundtrip_test PRIVATE quizpane_core Qt6::Core)
add_test(NAME attempt_roundtrip_test COMMAND attempt_roundtrip_test)
```

也就是说：

- 每个测试本质上是一个小程序；
- 返回 `0` 表示成功，非 `0` 表示失败；
- `ctest` 负责统一执行。

建议你从这些测试入手：

```text
tests/attempt_roundtrip_test.cpp          最简单，理解对象和 JSON 往返
tests/bank_validator_test.cpp             理解题库规则
tests/declarative_provider_flow_test.cpp  理解本地题库 Provider 请求
tests/chunker_test.cpp                    理解文档切块
tests/generation_workflow_test.cpp        理解模型调用 mock 和修复流程
```

跑单个测试：

```bash
build/dev/tests/attempt_roundtrip_test
```

跑全部测试：

```bash
ctest --test-dir build/dev --output-on-failure
```

## 10. 推荐学习路线

### 第 1 阶段：能看懂基本 C++ 文件

目标：不再被 `.hpp/.cpp`、引用、命名空间、Qt 字符串吓住。

阅读顺序：

```text
core/include/quizpane/attempt.hpp
core/src/attempt.cpp
tests/attempt_roundtrip_test.cpp
core/CMakeLists.txt
```

必须掌握：

- `struct`、`class`、`enum class`；
- `const`、`&`、`*`；
- 成员函数和静态函数；
- `QJsonObject`、`QJsonArray`；
- CMake target 的基本概念。

练习：

给 `Attempt` 增加字段，并让测试通过。

### 第 2 阶段：能读懂规则校验

目标：能像读 Java Validator 一样读 C++ 校验代码。

阅读顺序：

```text
core/include/quizpane/bank_validator.hpp
core/src/bank_validator.cpp
tests/bank_validator_test.cpp
schemas/examples/declarative-materials/content/bank.json
```

必须掌握：

- `QSet` 去重；
- `QHash` 建索引；
- `QRegularExpression`；
- 局部 helper 函数；
- 错误列表返回，而不是抛异常。

练习：

增加一个题目字段规则，配一个通过用例和失败用例。

### 第 3 阶段：能理解接口边界

目标：知道主程序如何和题库包隔离。

阅读顺序：

```text
sdk/include/quizpane/declarative_provider.hpp
sdk/src/declarative_provider.cpp
tests/declarative_provider_flow_test.cpp
tests/material_package_e2e_test.cpp
```

必须掌握：

- public/private；
- 类内状态；
- JSON request/response；
- 资源加载和卸载；
- integration test 如何覆盖完整链路。

练习：

给 `DeclarativeProvider::descriptor()` 多返回一个只读统计字段，例如材料数量，并加测试。

### 第 4 阶段：能读懂应用服务流程

目标：能顺着题库制作器后端状态机读完整流程。

阅读顺序：

```text
apps/bank-studio/engine/include/quizpane/studio/chunker.hpp
apps/bank-studio/engine/src/chunker.cpp
apps/bank-studio/engine/include/quizpane/studio/generation_workflow.hpp
apps/bank-studio/engine/src/generation_workflow.cpp
apps/bank-studio/engine/include/quizpane/studio/checkpoint_store.hpp
apps/bank-studio/engine/src/checkpoint_store.cpp
tests/generation_workflow_test.cpp
tests/checkpoint_store_test.cpp
```

必须掌握：

- 状态机；
- 断点续跑；
- 网络异步回调；
- mock 只放测试；
- DTO 和持久化结构的边界。

练习：

给 `WorkflowProgress` 增加一个只读统计项，并打通保存、进度发布和测试。

### 第 5 阶段：能做一个完整小需求

目标：从 schema、校验、运行时、测试完整走一遍。

建议需求：

“题库 material 支持 `difficulty` 字段，取值为 `easy|medium|hard`，Provider 能在 descriptor 里统计每种数量。”

你需要改：

```text
schemas/declarative-provider.schema.json
core/src/bank_validator.cpp
sdk/src/declarative_provider.cpp
tests/bank_validator_test.cpp
tests/declarative_provider_materials_test.cpp
schemas/examples/declarative-materials/content/bank.json
```

这个练习会逼你理解真实 C++ 项目的改动链路：schema -> domain validation -> runtime -> fixture -> tests。

## 11. 调试和定位问题的基本套路

### 11.1 编译错误

C++ 编译错误通常比 Java 长。先看第一条真正属于你改动文件的错误，不要被模板展开吓到。

常见类型：

- 找不到头文件：检查 `target_include_directories`；
- unresolved symbol：声明了函数但 `.cpp` 没实现，或 target 没链接；
- const 错误：在 `const` 函数里修改了成员，或把只读对象传给可写引用；
- 类型不匹配：`QJsonValue`、`QJsonObject`、`QJsonArray` 没转换清楚。

### 11.2 链接错误

如果看到 `undefined symbol`，优先检查：

- 函数签名是否和头文件完全一致；
- `.cpp` 是否加入对应 `CMakeLists.txt`；
- 测试 target 是否 `target_link_libraries` 到正确库。

Java 里很多问题是运行时 classpath 爆，C++ 里很多问题会提前在链接期爆。

### 11.3 测试失败

先跑单个测试，再跑全量：

```bash
build/dev/tests/bank_validator_test
ctest --test-dir build/dev --output-on-failure
```

如果是流程测试，先定位失败边界：

```text
输入 fixture 错？
校验规则错？
Provider 加载错？
答题请求错？
解析返回错？
```

不要一上来怀疑 UI。QuizPane 的很多业务问题都能在 `core`、`sdk`、`engine` 的测试里先定位。

## 12. 你需要补的 C++ 基础清单

按优先级补，不要从模板元编程开始。

第一优先级：

- `const`、引用、指针；
- 值语义、拷贝、移动的基本概念；
- `class`、`struct`、构造函数、析构函数；
- `enum class`；
- 头文件、源文件、链接；
- `std::vector`、`std::string`、`std::optional`、`std::unique_ptr`。

第二优先级：

- lambda；
- RAII；
- 错误处理风格：返回值、错误对象、异常；
- CMake target；
- Qt 的 `QObject`、信号槽、事件循环。

第三优先级：

- 模板；
- 智能指针深入；
- 多线程；
- ABI、动态库；
- 平台差异和打包。

## 13. Java Web 经验如何迁移

你的 Java Web 经验仍然很有用，尤其是这些判断：

- 分层是否干净；
- 业务规则是否应该在 domain/service，而不是 UI；
- 测试是否只 mock 外部边界；
- DTO 是否稳定；
- 持久化是否有版本和兼容策略；
- 错误信息是否足够定位；
- 流程是否能恢复、重试、幂等。

需要刻意切换的点：

- C++ 没有 GC，必须关心对象归属和生命周期；
- 编译链接是日常问题，不只是运行时问题；
- 头文件就是 API 合同，改头文件影响范围更大；
- 二进制依赖和平台差异比 Java 更明显；
- Qt 的事件循环不是 Web request/response 模型。

## 14. 最小上手命令

从干净工作区开始：

```bash
cmake --preset dev
cmake --build build/dev -j4
ctest --test-dir build/dev --output-on-failure
```

看文件：

```bash
rg "class GenerationWorkflow"
rg "validateBankDetailed"
rg "add_executable\\(bank_validator_test"
```

跑重点测试：

```bash
build/dev/tests/attempt_roundtrip_test
build/dev/tests/bank_validator_test
build/dev/tests/generation_workflow_test
```

## 15. 一条实际读源码路线

如果你明天就要开始读这个项目，按下面顺序来：

1. `README.md`：知道产品是什么；
2. `CMakeLists.txt`：知道模块怎么挂；
3. `core/include/quizpane/attempt.hpp`：看最简单领域对象；
4. `core/src/attempt.cpp`：看 JSON 往返；
5. `tests/attempt_roundtrip_test.cpp`：看最小测试；
6. `core/src/bank_validator.cpp`：看真实规则校验；
7. `sdk/src/declarative_provider.cpp`：看题库运行时；
8. `apps/bank-studio/engine/include/quizpane/studio/generation_workflow.hpp`：看业务流程接口；
9. `apps/bank-studio/engine/src/generation_workflow.cpp`：看状态机；
10. `tests/generation_workflow_test.cpp`：看如何测异步流程。

不要一开始读 UI 文件。Qt Widgets 代码会有大量布局、控件、信号连接，容易分散注意力。等你理解后端处理流程后，再回头看 UI 如何接入 workflow。

## 16. 推荐改代码姿势

每次改动都按这个顺序：

```text
1. 先找 public API 或 schema
2. 再找业务校验或 service 流程
3. 再找持久化/运行时
4. 最后补测试
5. 编译
6. 跑相关测试
7. 跑全量测试
```

在这个项目里，一个高质量 C++ 改动通常有这些特征：

- 业务规则在 `core` 或 `engine`，不是塞进 UI；
- mock 只在 `tests/*.cpp`；
- JSON 字段有校验、有测试、有示例；
- CMake target 依赖明确，没有全局 include/link 污染；
- 错误通过返回值或错误列表传递，调用方能展示或记录；
- 不引入“测试模式走另一条业务路径”。

## 17. 结论

你可以把 QuizPane 当成一个“没有 HTTP 的后端项目”来学 C++：`core` 是领域层，`sdk` 是运行时边界，`bank-studio/engine` 是应用服务，`tests` 是行为合同，CMake 是构建系统。先不要追求掌握所有 C++ 语法，先做到能顺着数据流读完整链路。

最推荐的第一周目标很简单：改一个 `core` 字段、补一个 validator 规则、写一个测试并跑通。做到这一步，你就已经跨过了从语法学习到工程学习的门槛。
