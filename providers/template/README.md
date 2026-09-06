# 第三方 Provider 最小模板

这是写一个新的原生 Provider（去远端拉题库的动态库）时的起点骨架，**不参与主项目构建**
（根 `CMakeLists.txt` 不 `add_subdirectory` 这个目录），只用来复制到你自己的仓库改。

如果你的题库是"平铺 JSON + 图片"，不需要登录/远程调用，优先用**声明式题库**
（`manifest.json + content/bank.json + assets/`，打包成 `.quizpane-provider`），
不需要写这份 C++ 模板、也不需要编译器。声明式题库的规则见
`schemas/declarative-provider.schema.json`。

只有当你确实需要"远程 HTTP 拉题、扫码登录、跨设备同步作答进度"这类动态能力时，
才需要写原生 Provider——也就是这份模板对应的形态。

## 这份模板和 `providers/demo` 的区别

`providers/demo/demo_provider.cpp` 是项目里**已注册进 CMake、被测试引用**的可运行示例
（`provider_loader_test`、`provider_flow_test` 依赖它），承担"回归测试夹具"的职责，
所以它的注释偏"这段业务逻辑做什么"，而不是"第三方开发者第一次抄这份代码要改哪里"。

`providers/template/provider_template.cpp` 只做一件事：**当骨架**。
业务数据被削减到最少（1 道写死的题），每个方法体内用注释标出"你需要把这里换成什么"，
不追求覆盖 demo 的全部演示场景（掌握度统计、多题分类等）。

## 你需要改的地方

1. `qp_provider_descriptor_json()` 里的 `id`/`name`/`version`——`id` 要全局唯一，
   建议用反向域名（如 `com.yourcompany.yourbank`）。
2. `handleRequest()` 里每个 `method` 分支——把写死的返回值换成你的真实数据源
   （HTTP 请求、本地数据库等）。
3. 如果需要保存登录态/Token，用 Host 注入的 `host->secure_write/secure_read`，
   不要自己写文件或调用系统 Keychain/DPAPI——那是 Host 的职责，见
   `sdk/include/quizpane/provider_abi.h` 里 `qp_host_api_v1` 的注释。
4. `qp_provider_request` 目前是**同步**返回（收到请求就在同一次调用里回调），
   真实网络请求应该发起异步 HTTP 后立刻返回、在网络回调里再调用 `callback`；
   `handle` 需要能安全地在其他线程被 `qp_provider_cancel`/`qp_provider_destroy`
   访问，注意加锁或用线程安全的队列。

## 需要实现的最小方法集

按 Host 实际会调用的顺序：

| method | 时机 | 你需要返回什么 |
|---|---|---|
| `provider.initialize` | 题库首次加载/每次启动 | 是否需要登录、会话是否已恢复 |
| `provider.capabilities` | 初始化之后 | 登录方式、每次组卷可选题量 |
| `catalog.list` | 打开分类列表 | 分类树 + 每类可用题数 |
| `attempt.create` | 用户开始一次练习 | 新建的 attemptId、题量 |
| `attempt.get` | 恢复未完成的练习 | 已有 attempt 的当前状态 |
| `attempt.questions` | 拿到 attempt 后 | 题干、选项（不含答案） |
| `attempt.saveAnswers` | 用户作答/翻页时 | 确认保存成功 |
| `attempt.submit` | 用户交卷 | 确认交卷成功 |
| `attempt.report` | 交卷后 | 正确数、耗时等统计 |
| `attempt.solutions` | 查看解析 | 每题的正确答案与解析文本 |

字段结构直接照抄 `provider_template.cpp` 里已经写好的 JSON 形状，不需要另外发明协议。
