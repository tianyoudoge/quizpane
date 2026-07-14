# 阶段 4/5 后端处理流程 Code Review

本文面向熟悉 Java Web 分层架构、DTO、Service、Repository 和外部网关的开发者，说明材料题库从源文档到可安装 Provider 的后端链路，以及评审时应重点确认的不变量。

## 1. 先看整体调用链

```text
源文件
  -> ExtractorRegistry              文档适配与纯文本提取
  -> Chunker.blocks()               识别稳定 SourceBlock
  -> Chunker.split()                装配模型请求 TextChunk
  -> GenerationWorkflow            串行编排生成、校验、修复
       -> ModelClient               OpenAI-compatible HTTP 网关
       -> BankValidator             Schema 与跨对象引用校验
       -> CheckpointStore           v2 检查点原子持久化
  -> GeneratedBankCandidate         materials/questions/review DTO
  -> Provider 打包与安装自检
       -> ProviderInstaller.inspect()
       -> ZIP 重新读取
       -> DeclarativeProvider.load()
```

如果用 Java Web 类比：`GenerationWorkflow` 是 application service，`ModelClient` 是 outbound gateway，`CheckpointStore` 是 repository，`BankValidator` 是无状态 domain service，`GeneratedBankCandidate` 是跨层 DTO。网络响应不会直接进入持久化或打包，必须先经过 DTO 解析和统一规则校验。

## 2. 文档切块：恢复单位与请求单位分离

关键文件：

- `apps/bank-studio/engine/include/quizpane/studio/chunker.hpp`
- `apps/bank-studio/engine/src/chunker.cpp`

`SourceBlock` 和 `TextChunk` 是两个不同层次的对象：

- `SourceBlock` 是业务恢复单位，拥有任务内稳定的 `index`；
- `TextChunk` 是一次模型请求，可能包含多个普通 `SourceBlock`；
- `TextChunk::sourceBlockIndices` 建立请求与恢复单位的映射。

这相当于批处理系统把“业务 item”与“HTTP batch”分开。检查点提交业务 item，不提交偶然形成的请求 batch，因此以后调整 token 预算时不会把已经完成的业务进度误认成另一段文本。

`Chunker::blocks()` 先识别强边界：

- “根据以下资料，回答第 1～3 题”等题组提示；
- “材料一 / 材料二”等材料标题；
- Markdown 标题分节；
- 连续编号并带 A～D 选项的完整题目。

材料题组被标记为 `indivisible`。即使估算 token 超预算，也优先保持材料与子题完整；静默拆开会使模型失去 `materialId` 的上下文，属于业务错误，不是普通的性能问题。

评审重点：

1. `startIndex` 是否在多文件任务中持续递增；
2. 任一不可拆块是否只出现在一个 `TextChunk`；
3. 普通超长段落是否仍按预算拆分，避免无界请求；
4. 新增边界规则是否基于确定性文本信号，而不是模型猜测。

## 3. 生成状态机：解析、校验、修复

关键文件：

- `apps/bank-studio/engine/include/quizpane/studio/generation_workflow.hpp`
- `apps/bank-studio/engine/src/generation_workflow.cpp`
- `apps/bank-studio/engine/include/quizpane/studio/model_client.hpp`

模型协议固定返回：

```json
{
  "materials": [],
  "questions": []
}
```

没有共享材料时 `materials` 也必须存在并为空数组。`parseGeneratedBankCandidate()` 只负责 JSON 边界和顶层 DTO 形状，不做网络操作，也不修改工作流状态；其职责类似 Jackson 反序列化后的 request DTO 校验。

每个块的状态迁移如下：

```text
Generating
  -> parse 失败 ---------> Repairing
  -> parse 成功
       -> BankValidator 失败 -> Repairing
       -> BankValidator 成功 -> completeChunk

Repairing
  -> 成功 -> completeChunk
  -> 仍失败 -> 标记 needsReview -> completeChunk
```

修复请求不是只发送某一道题。请求体包含：

- 当前 `SourceBlock` 原文；
- 上一轮完整 `materials + questions` 输出；
- 包含 `questionIndex`、`questionId`、`materialId`、`message` 的结构化错误。

这样模型能够同时修复材料对象和全部关联子题。只回传单题会破坏聚合边界，类似更新订单明细时完全不提供订单头和其他关联明细。

第二轮仍不合法时，不把数据伪装成“已通过”，也不静默丢弃。可解析的题目进入 `needsReviewQuestions`，并保存失败原因；最终产物仍必须再次通过 `BankValidator`，因此断裂引用或非法材料不可能绕过质量门槛。

评审重点：

1. 生产代码中不得出现测试模式、固定模型响应或 `#ifdef TEST`；
2. token 统计只累计模型响应的 usage，不使用字符估算冒充计费数据；
3. 修复请求必须包含上一轮完整候选和材料上下文；
4. 网络失败、解析失败、规则失败必须是不同错误路径；
5. 完成信号只发布 `GeneratedBankCandidate`，不再维护容易错位的平行数组参数。

## 4. 稳定 ID 与跨对象引用

模型生成的 ID 只在当前响应内可信。`completeChunk()` 在持久化前执行稳定重命名：

```text
材料：b{sourceBlock}-m{ordinal}-{modelId}
题目：b{sourceBlock}-q{ordinal}-{modelId}
```

材料重命名保存于 `materialIdRenames`，key 为 `源块序号:模型材料ID`。题目的 `materialId` 必须通过同一映射同步改写，不能分别生成。`materialQuestionIds` 保存最终材料 ID 到最终题目 ID 的关联，用于恢复后重建聚合关系。

这里需要维持三个不变量：

1. 同一源块重放时得到相同材料 ID；
2. 不同源块即使模型返回相同 ID，也不会冲突；
3. 题目引用与材料 ID 必须在同一原子提交中一起变更。

## 5. 检查点 v2：严格读取，不猜迁移

关键文件：

- `apps/bank-studio/engine/include/quizpane/studio/checkpoint_store.hpp`
- `apps/bank-studio/engine/src/checkpoint_store.cpp`

v2 保存以下核心状态：

- 源路径与 SHA-256 fingerprint；
- `sourceBlockCount` 与 `completedSourceBlocks`；
- `materials`、正常题、待复核题；
- `materialIdRenames` 与 `materialQuestionIds`；
- 模型输入、输出 token 累计值。

写入使用 `QSaveFile`，语义类似“写临时文件后原子 rename”，避免进程中断留下半份 JSON。文件权限限制为当前用户读写。

加载采用 fail-closed 策略：

- `version != 2`：返回“任务结构已升级，请重新开始”；
- v2 必填字段类型不符：同样拒绝；
- 源文件摘要变化：拒绝复用；
- 重新切块后的 `sourceBlockCount` 不一致：拒绝复用。

旧检查点只有题目数组，没有材料聚合关系。自动迁移必须猜测哪些题共享材料，这会制造不可验证的数据，因此明确不迁移。该取舍相当于数据库变更中拒绝编写无法保证语义正确的数据回填脚本。

评审重点：

1. `save()` 新增的字段是否在 `load()` 中严格对称；
2. 保存是否保持原子性；
3. completed 集合是否以 `SourceBlock` 为单位；
4. 旧版本或内容不完整时是否明确失败，而不是返回部分对象。

## 6. 最终质量门槛与端到端验证

最终 JSON 只有一个 `schemaVersion: 2`，材料支持通过 `materials` 和题目可选 `materialId` 表达。校验器、安装器和运行时都直接拒绝 v1，不存在兼容或迁移分支。

打包链路必须依次验证：

1. `BankValidator` 校验材料唯一性、引用存在性、分类一致性、孤立材料及正文重复；
2. 写出 `.quizpane-provider`；
3. `ProviderInstaller::inspect()` 从包内检查 manifest；
4. 从 ZIP 重新读取 `content/bank.json`；
5. `DeclarativeProvider::load()` 加载重新读取的文件；
6. 执行 `attempt.create`、`attempt.questions`、`attempt.submit`、`attempt.solutions`；
7. 保存并恢复含 `materials` 的 `DraftSnapshot`。

`tests/material_package_e2e_test.cpp` 使用仓库内固定 fixture 覆盖上述确定性链路。真实模型只用于人工压测，不对题数或措辞做 CTest 断言。

## 7. 测试边界

确定性测试分为三层：

- `chunker_test.cpp`：固定文本验证材料题组不可拆；
- `generation_workflow_test.cpp`：测试文件内的本地 HTTP server 依次返回错误引用和修正响应，验证修复状态机；
- `checkpoint_store_test.cpp`：验证 v2 round-trip、源文件变化拒绝、v1 明确拒绝；
- `material_package_e2e_test.cpp`：验证打包、安装、加载、答题、解析和草稿恢复。

所有 mock 都只存在于 `tests/*.cpp`。生产路径只有真实 HTTP gateway、纯解析器、规则校验器和持久化 repository，不存在测试专用分支。

## 8. 建议 Review 顺序

1. 先看 `GeneratedBankCandidate` 和 `GenerationCheckpoint`，确认数据边界；
2. 再看 `Chunker::blocks()` / `split()`，确认恢复单位；
3. 按 `handleModelResult()` 的状态迁移读生成与修复；
4. 看 `completeChunk()` 的 ID 重命名与原子提交；
5. 对照 `CheckpointStore::save()` / `load()` 检查字段对称；
6. 最后从四个测试反推失败语义和质量门槛。
