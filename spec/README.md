# QuizPane 产品关键逻辑 Spec

> 本目录是 QuizPane（题库制作器 bank-studio 为核心）的**关键逻辑参考文档**，面向 vibe coding：
> 每一条结论都尽量给出 `file:line` 代码位置，方便在改代码前快速定位上下文。
> 代码位置基于 2026-08- 的工作区状态，行号可能随代码演进漂移，函数名比行号更稳定。

## 目录

| 文件 | 内容 |
|---|---|
| [01-总览.md](01-总览.md) | 应用结构、数据流全景、核心类型（ExtractedDocument / ReviewResult / bank.json） |
| [02-文档提取与扫描件处理.md](02-文档提取与扫描件处理.md) | PDF 文字层 vs 扫描页判定、OCR、版面锚点、下划线检测 |
| [03-规则识别链路.md](03-规则识别链路.md) | 套题/单套切分、切题算法、四种答案位置、题型识别、三级复核、已知坑 |
| [04-智能识别链路-MinerU.md](04-智能识别链路-MinerU.md) | MinerU 服务接入、输出格式、后处理、与规则链路的关系 |
| [05-识别后手工编辑与导出.md](05-识别后手工编辑与导出.md) | 复核 UI、图片区域重裁（CropDialog）、可编辑字段矩阵、草稿校验、打包导出 |

## 一句话架构

```
源文件(txt/md/docx/pdf)
  → 文档提取 ExtractedDocument（纯文本 + 归一化版面锚点 + 按需页面 PNG）
      ├─ 本地提取：TXT/DOCX 直读；PDF 逐页判定文字层/扫描页(OCR)
      └─ 云提取（可选）：MinerU 服务解析 ZIP → 适配成同一个 ExtractedDocument
  → 规则引擎 RuleBasedBankGenerator（纯离线、确定性、无网络）
      → ReviewResult（questions / needsReviewQuestions / materials / 附件）
  → 复核 UI（QTreeWidget + 详情编辑器 + CropDialog 图片重裁）
  → 打包 .quizpane-provider（manifest + bank.json + assets/），安装给小窗刷题
```

关键设计约束（写代码前必读）：

1. **规则引擎是纯函数**：相同输入必须相同输出；不依赖网络/窗口/检查点（`rule_based_generator.hpp:12-19`）。
2. **MinerU 是上游提取后端，不是 fallback**：不存在"规则失败后偷偷调模型"的混合路径（`generation_workflow.cpp:18-147`）。
3. **宁缺毋滥**：识别不出答案/选项的题进 `needsReviewQuestions` 人工复核，禁止用猜测答案换 Schema 表面合法（`review_result.hpp:11-14`）。
4. **题目全链路是 `QJsonObject`**：没有 C++ Question struct，字段契约以 C++ 校验器 `core/src/bank_validator.cpp` 为权威（JSON Schema 文件滞后）。
5. **原 PDF 永不修改**：一切"改图"都是临时重渲染 PDF 页 + 新框裁 PNG。
6. **一个题库只能"含答案"或"无答案"，不能混合**（`review_result.hpp:20-21`）。

## 核心代码位置速查

| 主题 | 位置 |
|---|---|
| 规则引擎入口 | `apps/bank-studio/engine/src/rule_based_generator.cpp:2191`（`generate`） |
| 文档提取 | `apps/bank-studio/engine/src/document_extractor.cpp`（PDF 提取 `:544`） |
| MinerU 客户端 | `apps/bank-studio/engine/src/mineru_client.cpp` |
| MinerU 输出适配 | `apps/bank-studio/engine/src/mineru_output_adapter.cpp` |
| 生成工作流 | `apps/bank-studio/engine/src/generation_workflow.cpp` |
| 中间结果结构 | `apps/bank-studio/engine/include/quizpane/studio/review_result.hpp:13` |
| 复核/编辑 UI | `apps/bank-studio/src/studio_window.cpp`（3112 行） |
| 题库校验器 | `core/src/bank_validator.cpp`（题型白名单 `:23`） |
| 导出打包 | `studio_window.cpp:2841`（`packageProvider`） |
| bank.json Schema | `schemas/declarative-provider.schema.json`（运行时以 C++ 校验器为准） |
| 规则引擎测试 | `tests/rule_based_generator_test.cpp`（1502 行）、`tests/duplicate_question_number_test.cpp` |

## 相关历史文档（docs/）

- `docs/规则结构化题库后端处理流程CodeReview.md` — 规则链路设计说明（部分已滞后，以代码为准）
- `docs/MinerU替换AI视觉链路方案.md` — MinerU 链路方案
- `docs/题库生成器UI与集成方案.md` — UI 四步流程
- `docs/booklet-recognition-regression.md` — 套题切分回归（147 页真实样本）
- `docs/embedded-answer-recognition.md` — 题内嵌答案识别
- `docs/共享材料与题组支持交接方案.md` — 材料/题组
