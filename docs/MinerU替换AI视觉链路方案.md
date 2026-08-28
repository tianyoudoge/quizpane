# MinerU + 启发式替换 AI 视觉链路方案

> 日期：2026-08-22
> 前置调研：《MinerU 复杂真题解析接入调研》（Day9 样本，2026-08-17）
> 本轮补充实测：`数资Day11电子版合集（2027）.pdf`，10 页，MinerU 精准 API（vlm）
> 本轮代码勘察：`apps/bank-studio/` 全量链路（结论见 §1.1、§5）

## 1. 决策

**下掉视觉 LLM（VLM）链路，题库制作器只保留两条解析路径 + 一个终点：**

```text
1. 本地启发式（QPdfDocument 文字层 + 规则引擎）
   —— 零配置、离线可用，处理简单文字卷

2. MinerU 精准 API
   —— 唯一需要配置的 sk，处理扫描件 / 图表 / 图片选项 / 复杂表格

3. 两路都搞不定的题 → needsReviewQuestions 硬复核
   —— 人工修，不是 AI 修
```

决策动机：

- **用户不应配置两个 sk。** 原方案中"MinerU 解析 + 视觉 LLM 兜底"意味着两套凭据、两套模型配置 UI，配置复杂度对个人用户不可接受。
- **VLM 承担的活基本能被 MinerU + 简单规则接住**（见 §3 逐项对照）。
- **VLM 兜底"结构性缺失"本身不可靠。** 缺选项、答案数不符这类问题让 LLM 修，容易产出看似合法但语义错误的题；人工复核是更诚实的终点。这也是前置调研第 10.5 条"失败显式化"原则的彻底执行。

用户视角的最终形态：

- 不配任何 key → 可用，仅本地解析；
- 配一个 MinerU Token → 解锁复杂卷云解析；
- 设置页只有一个 Token 输入框，附一句"不填也能用，仅本地解析"。

### 1.1 代码勘察修正：现状比预想更有利

对 `apps/bank-studio/` 的全量勘察确认了一个重要事实：**当前生成主链路本来就是
100% 离线规则引擎，AI（VLM）从未参与生成**。`GenerationWorkflow` 只有
`startRuleBased` 一个入口（`generation_workflow.hpp:48`），注释明写"完全离线的
规则结构化工作流，没有网络请求"。LLM 只在复核页作为两个**用户手动点击**的
辅助按钮存在：

- AI 复核建议（`studio_window.cpp:2054` `requestAiReview()`）：单题 JSON 纯文本审计，建议只回填草稿框，永不直接落库；
- AI 修正裁切（`studio_window.cpp:1535` `requestAiCrop()`）：局部 PNG 视觉定位，结果回填成可拖拽选区供人工确认。

因此本方案的实质是：

1. **"用 MinerU 替换 VLM 生成链路"不成立——没有 VLM 生成链路可替换**。真正被替换/增强的是 `PdfExtractor` 的文档感知层（QPdfDocument 文字层 + Tesseract OCR + 像素级下划线检测）；
2. 要删的 AI 面积很小且边界清晰：`ModelClient`（`model_client.*`）、模型配置对话框（`model_settings_dialog.*`）、复核页两个 AI 按钮及其分派逻辑（`studio_window.cpp:545` 的 `aiCropInFlight_` 布尔路由）、SecretStore 里的模型 API Key；
3. 顺手清理三处死代码：`Chunker`（`chunker.hpp`）、`CheckpointStore`（`checkpoint_store.hpp`）——旧"整份文档发模型"路径的残骸，生产零调用只有测试引用；`extractOptionImages()`（`rule_based_generator.cpp:433`）——逐选项裁图能力写完了但零调用；
4. "AI 修正裁切"删除后，其职责由 MinerU 的 bbox 天然承接（MinerU 直接给出图块坐标，比让 LLM 看图猜坐标更准），人工拖拽重裁（`recropReviewAsset()`，纯本地）保留为最终手段。

## 2. 两轮样本实测证据

### 2.1 Day11 实测（本轮，vlm 模型）

流程：`POST /api/v4/file-urls/batch` 申请预签名链接 → `PUT` 上传 → 轮询
`GET /api/v4/extract-results/batch/{batch_id}` → 下载 `full_zip_url`。
10 页 PDF 解析用时 < 10 秒。

产物：`full.md`、`content_list.json`（93 个块：text 50 / header 17 /
table 7 / chart 5 / image 4 / page_number 10，均带 `page_idx`）、
`layout.json`、16 张切图。

| 检查项 | 结果 | 判断 |
|---|---|---|
| 题号 111–130 | 20/20 全部出现 | 通过 |
| 参考答案 | `CAACD DBABC BBCCD DABAC` 完整 20 位 | 通过 |
| 资料分析统计图 | 识别为独立 `chart` 块 + bbox + 切图 | 通过（对刷题场景，图表切成图正是需要的形态） |
| 数据表 | 还原为 HTML table，数值基本准确 | 通过（保留原图复核） |
| 选项粘连 | `A. 5 B. 4C. 3 D. 2`（B/C 挤在一行） | 需启发式再拆，规则可修 |
| 页脚污染 | 第 130 题选项行为 `新浪微博：公考齐麟 …微信公众号：公考齐麟A. 0项 B. 1项…`，页脚文本粘进选项行 | 需文本级前缀/中缀清洗，规则可修 |
| 漏选项 | 第 124 题只出现 A、B 两个选项 | **规则不可修 → 硬复核** |

### 2.2 与 Day9 实测的交叉验证

Day9（前置调研）与 Day11 暴露的缺陷模式高度一致，说明是**稳定模式而非偶发**：

- 选项粘连：两轮均出现 → `optionsOnLine` 类规则拆分即可；
- 页眉页脚污染：Day9 是"跨页选项被误标 header"，Day11 是"页脚文本粘进选项行"，
  说明**光丢弃 header/footer 块不够**，还要做文本级清洗 + "页首疑似选项回看上一题"规则；
- 结构性缺失：Day9 第 115 题漏 C、Day11 第 124 题缺 C/D → 只能硬复核；
- 图片选项题（Day9 第 129 题）：四图四标签独立 bbox，切图完整无错位 → **MinerU 实质性增强，验证通过**；
- 跨页题干图（Day9 第 130 题）：chart 找到但文字选项被吞进图 → 需本地文字层补回。

按 Day11 比例估算，MinerU + 启发式后真正落到人工复核的题约 **5% 量级**
（20 题中 1 题缺选项不可修、1 题页脚污染可由规则修），对制作者场景可接受。

## 3. 为什么砍掉 VLM 是安全的

逐项对照原来 VLM 承担的职责：

| 原 VLM 场景 | 现在谁接 | 证据 |
|---|---|---|
| 图片选项题定位切图 | MinerU：图 + 标签独立 bbox | Day9 §3.3，第 129 题验证通过 |
| 扫描件 OCR | MinerU `is_ocr` / vlm 模型 | 官方能力，成本远低于调 LLM |
| 统计图表识别 | MinerU `chart` 块 + 切图 | Day11 实测通过 |
| 选项粘连拆分 | 启发式 `[A-F][\.．、]` 切分 | 现有 `optionsOnLine` 规则 |
| 页眉页脚污染 | 启发式：块类型过滤 + 已知文案文本清洗 | 纯字符串规则 |
| 缺选项 / 答案数不符 | **硬复核，人工补录** | VLM 修这类本就易编造 |

关键认知：VLM 唯一"独占"的能力是兜底结构性缺失，而这恰恰是它最不可靠的场景。
换成人工复核，失败从"静默产出错题"变成"显式待办"，质量下界反而更高。

## 4. 目标架构

### 4.1 数据流（对齐现有模块名）

```text
用户拖入 PDF   [studio_window.cpp dropEvent -> appendSources]
  -> 本地预检（大小、页数、有无文字层、隐私确认）
  -> 路由（二选一，默认本地；云解析必须用户确认）
       简单文字卷              -> 现有 PdfExtractor（QPdfDocument + Tesseract）
       扫描页/图片选项/复杂版面  -> MineruExtractionJob（异步，先于 workflow 跑完）
                                   申请上传 URL -> PUT -> 轮询 -> 下载 ZIP -> 校验解压
  -> MineruOutputAdapter
       content_list.json（主入口）/ model.json（诊断补救）
           -> LayoutBlock[]（适配器内部表示）
           -> 反向填充 ExtractedDocument:
                plainText（\f 分页，与本地路径同构）
                questionAnchors / optionLabelAnchors / lineAnchors（bbox 归一化 0..1）
                pageImages（MinerU 切图或按 bbox 从原 PDF 高分辨率重裁）
       full.md 仅预览诊断，不参与图片绑定
  -> GenerationWorkflow::startRuleBased(...)        [现有，签名不变]
  -> RuleBasedBankGenerator::generate(...)          [现有 2019 行规则引擎，零改动起步]
  -> 缺项/冲突 -> needsReviewQuestions 硬复核        [现有三层信号机制]
  -> validateBank() -> writeZipArchive -> .quizpane-provider   [现有，不可绕过]
```

关键性质：**MinerU 路径与本地路径在 `ExtractedDocument` 这一点汇合**，其下游
（规则引擎、复核页、打包校验）完全不改。本地启发式因此不是"降级路径"，而是
同一条管线换个感知输入——回退零成本。

### 4.2 供应商无关中间层

`LayoutBlock` 定位为**适配器内部表示**，不进入 `ExtractedDocument` 的公开契约
（首版），避免规则引擎出现两套数据源分支：

```cpp
enum class LayoutBlockType {
    Text, Title, Image, Table, Chart, Equation, Header, Footer, PageNumber, Unknown
};

struct LayoutBlock {
    LayoutBlockType type;
    int page = 0;                // 统一 1-based（MinerU page_idx 为 0-based）
    QRectF normalizedBounds;     // 统一归一化 0..1（MinerU bbox 为 0..1000）
    QString text;
    QString assetPath;
    double confidence = -1.0;
};
```

`ExtractedDocument` 只新增一个诊断字段：

```cpp
QString extractionBackend;   // "local-qt" | "mineru-vlm" | "mineru-pipeline"
```

MinerU 的 pipeline 与 vlm 坐标系不同，**归一化必须在适配器入口完成**；规则引擎
不应知道任何 MinerU 原始字段名。待 Phase 3 回归稳定后，再评估是否把
`layoutBlocks` 提升为公开字段供规则引擎直接消费。

### 4.3 组装规则要点（映射到现有函数）

| 规则 | 落点 | 来源 |
|---|---|---|
| header/footer/page_number 块先过滤，**再对正文做已知页眉页脚文案的前缀/中缀清洗** | 适配器内（不进 plainText）+ `sourceLines()`（`rule_based_generator.cpp:767` 已有页码行剔除，扩展文案黑名单） | Day11 第 130 题：页脚粘进选项行 |
| 页首疑似选项（命中 `^[A-F][\.．、]` 且上一页选项序列未完成）回看上一题，不直接丢弃 | 适配器内：header 块不无条件丢弃，降级为普通文本行 | Day9 第 115 题：C 选项被误标 header |
| 选项同行粘连再拆 | `optionsOnLine()`（`rule_based_generator.cpp:1161`，现成能力） | Day9/Day11 均出现 |
| 图片选项绑定不满足 `A/B/C/D = 4/4` → 硬复核，绝不把整页图当选项 | `parseQuestion()` 的 `reasons` 累积（`:1450-1475`） | Day9 §4.4 |
| 答案仅在题号连续且数量完全相等时按位置映射 | `globalAnswers()`（`:970`）+ 现有顺序配对约束（`:1923`，已要求数量恰好相等） | 现有行为已合规 |
| 静默丢失 = 0 | 现有 hard/soft 双层 + `applyRuleBasedGenerationAudit()`（`:1610`） | 现有机制沿用 |

裁图策略沿用现有决策：最终资产**按 bbox 从原 PDF 高分辨率渲染图裁切**
（`ensurePdfPageImages` + `extractQuestionVisualImage`），而不是盲信 MinerU
导出的压缩图——MinerU 的 bbox 用作定位证据，像素来源仍是原卷。

## 5. 改造面（对照实际代码）

### 5.1 接入点与现状约束

实际代码只有一层多态接口：`DocumentExtractor`
（`document_extractor.hpp:59`，三个实现 Txt/Docx/Pdf）。MinerU 后端属于
"文件 → ExtractedDocument"这一层，接入约束如下：

- **`ExtractorRegistry` 是硬编码成员 + if-else 链**（`document_extractor.cpp:616-627`），不是可注册容器。加 MinerU 后端要么扩这条链，要么先小改成容器——首版扩链即可；
- **`ExtractedDocument` 是纯 PDF 语义结构体**：`plainText`（`\f` 分页）+ `questionAnchors` / `optionLabelAnchors` / `lineAnchors`（`QHash<int, QList<PdfTextAnchor>>`，bbox 已归一化 0..1）+ `pageImages` + `underlineDecorations`。**MinerU 适配器必须反向填充这些 anchors**（题号块→questionAnchors、`^[A-F][\.．、]` 命中→optionLabelAnchors、文本块逐行→lineAnchors），否则题图/公式选项/材料裁切静默失效——这正好与 §4.2 "第一版由 MinerU 文字块反向填充现有 anchors" 的策略一致；
- **`GenerationWorkflow` 在 `QtConcurrent::run` 工作线程里同步调 `registry.extract()`**（`generation_workflow.cpp:64`）。MinerU 是异步任务（上传→轮询→下载），不能塞进这个同步调用：`MineruExtractionJob` 作为独立状态机在 workflow 之前跑完，产出 ZIP 本地路径后再进现有同步管线；
- engine 库已 `PUBLIC` 链接 `Qt::Network`（原为 `model_client` 引入），`mineru_client` 依赖上无新增；
- 最终产物契约不变：`ReviewResult`（`review_result.hpp:12`）→ `validateBank()`（`core/src/bank_validator.cpp`，不可绕过）。

### 5.2 改造清单

| 动作 | 模块 | 职责 |
|---|---|---|
| 新增 | `mineru_client.*` | Token 鉴权、签名上传、轮询、取消、超时、下载 |
| 新增 | `mineru_output_adapter.*` | ZIP 校验解压（防 ZIP Slip/炸弹）；content_list.json → 反向填充 `ExtractedDocument` anchors + `pageImages` |
| 新增 | `MineruExtractionJob` | 异步状态机：uploading/pending/running/downloading/adapting；在 GenerationWorkflow 之前完成，不改其同步语义 |
| 修改 | `document_extractor.hpp/cpp` | `ExtractorRegistry` 增加 MinerU 分支；`ExtractedDocument` 增加 `extractionBackend` 元数据 |
| 修改 | `generation_workflow.*` | 增加远程解析阶段进度透传（复用现有 `WorkflowStage` 或前置） |
| 修改 | `secret_store` 用法 | MinerU Token 用独立命名空间；沿用现有"按需读钥匙串"模式（`ensureModelApiKeyLoaded()` 同款） |
| 修改 | 制作器 UI | 单一 Token 输入框；"本地/云"选择与上传确认提示 |
| **删除** | `model_client.*` | 唯一的 LLM 传输层，OpenAI-compatible Chat Completions |
| **删除** | `model_settings_dialog.*` | 五厂商模型配置 UI（openai/anthropic/dashscope/zhipu/ollama） |
| **删除** | `studio_window.cpp` 中 `requestAiReview` / `requestAiCrop` / `handleAiReviewResult` / `handleAiCropResult` 及 `aiCropInFlight_` 分派 | 复核页两个 AI 按钮；人工拖拽重裁（`recropReviewAsset()`）保留 |
| **删除** | `chunker.*`、`checkpoint_store.*`、`extractOptionImages()` | 死代码顺手清理（前两者含对应测试） |
| 修改 | README | "模型配置"相关文案同步更新 |

注意保留：`PdfExtractor` 全部保留（本地路径主力）；`detectPdfUnderlinesForCandidateLines`
像素级下划线检测保留（MinerU 不输出下划线样式，材料划线题仍靠它）；
`needsReviewQuestions` 三层复核信号机制（hard/soft/批量审计）原样沿用。

MinerU 路径下已激活 `option.image`（schema 与 `parseQuestion` 原本已有写入契约）：
只有四个标签 bbox 完整、同行且横向有序时才生成 A–D 四张独立选项图；任一条件
不满足就回退为整题 `stemImage` 并进入复核，绑定规则遵守 §4.3，不猜测缺失边界。

## 6. MinerU API 备忘（2026-08 官方口径）

- 鉴权：`Authorization: Bearer <token>`，Token 在 mineru.net「API 管理」页创建；
- 本地文件唯一上传方式：`POST /api/v4/file-urls/batch` 申请预签名链接（≤50 个/次，24h 有效）→ `PUT` 上传（**不要带 Content-Type**）→ 上传即自动开始解析；
- 轮询：`GET /api/v4/extract-results/batch/{batch_id}`，state:
  `waiting-file → pending → running → converting → done/failed`；
- 参数：`model_version=vlm`（推荐）、`is_ocr`、`enable_formula`（LaTeX 输出）、`enable_table`（HTML 输出）、`language=ch`、`page_ranges`；
- 额度：免费每天约 2000 页高优先级，超出**降级排队而非拒绝**；单文件 ≤200MB / 600 页。产品内**不硬编码额度数字**，文案写"以账号控制台为准"，对 429/长 pending 做指数退避；
- 隐私：文件经签名 URL 上传至其 OSS/CDN，官方无明确保留周期/不训练承诺 →
  上传前必须显式告知并经用户确认，默认不自动上传；Token、签名 URL、原文不进诊断日志。

## 7. 分阶段计划

> **实施进度（2026-08-23）**：Phase 1、Phase 2 已完成，见文末《实施记录》。

- **Phase 0（已完成）**：Day9 + Day11 双样本 PoC + 全量代码勘察。精准 API 流程跑通，缺陷模式收敛且可分类（规则可修 / 硬复核）；确认生成主链路本就离线、AI 面积小且边界清晰（§1.1）。

- **Phase 1 — 适配器 + 固定回归（2–4 天，纯离线可测）**
  - `mineru_output_adapter`：ZIP 校验解压（防 ZIP Slip / 压缩炸弹）→ `LayoutBlock[]` → 反向填充 `ExtractedDocument`；
  - 用**已下载的 Day9/Day11 ZIP 作为离线 fixture**，不依赖网络即可跑测试；
  - 两卷同时做 golden JSON（缺陷面互补：Day9 有图片选项题 129、跨页 130；Day11 有页脚粘选项、第 124 题缺 C/D）；
  - 断言含裁图尺寸/哈希；
  - 不动 UI、不发网络请求。

- **Phase 2 — 云任务工作流 + 删 AI（3–5 天）**
  - `mineru_client` + `MineruExtractionJob`：上传、轮询、取消、超时、指数退避；
  - Token 接 `SecretStore`（独立命名空间，沿用按需读钥匙串模式）；
  - 制作器 UI：单 Token 输入框 + 本地/云后端选择 + 上传前显式确认；
  - **同期执行 §5.2 的删除项**（`model_client`、`model_settings_dialog`、复核页两个 AI 按钮、死代码三处）。删除与新增同版本落地，避免出现"两个 sk 并存"的中间态。

- **Phase 3 — 路由与体验（核心收尾已完成）**
  - 复杂版面自动检测：只**建议**云解析，绝不自动上传；
  - 已确认复核页支持"缺选项补录"（Day11 第 124 题这类硬复核可直接落地）；
  - 已激活 `option.image`（完整四标签才裁四图，否则安全回退）；
  - 按后端统计准确率与耗时。

## 8. 验收标准（以 Day11 为例）

| 指标 | 通过线 |
|---|---:|
| 题号 111–130 完整率 | 20/20 |
| 每题题干非空 | 20/20 |
| A–D 选项完整（含硬复核补录后） | 80/80 |
| 自动解析直出选项完整率 | ≥ 76/80（缺项必须全部显式进复核，0 静默丢失） |
| 答案映射 | 20/20，与 `CAACD DBABC BBCCD DABAC` 一致 |
| 共享材料绑定 | 每组只绑定到对应题目 |
| 页眉页脚污染进入题干/材料/选项 | 0 条 |
| 任务元数据 | MinerU 模型版本写入诊断元数据（云端模型升级后可解释回归变化） |

Day9 对应答案串为 `DBCAC ABACD BCDBA ABADB`，另需断言第 129 题四张独立选项图
A–D 无错位、第 130 题跨页题干图 + 四个文字选项归属正确。

另需补充 fixture：扫描卷、双栏卷、答案另册、无答案卷、题号不连续、
图片选项跨页、表格跨页。

## 9. 风险与取舍

- **没有 VLM 后，双路都失败的题 100% 落人工**。按 Day11 实测约 5% 量级，制作者场景可接受；题库最终质量本就需要人过目。
- MinerU 云端模型会静默升级 → golden fixture + 模型版本元数据是回归的唯一防线。
- 完全离线用户只有本地启发式，复杂卷体验下降 → 既定取舍，文案说清楚。
- **删除 AI 复核按钮会影响已依赖它的用户**（虽然它只回填草稿框，不落库）→ 发版说明里明确说明，并指出人工复核 + 拖拽重裁能力完整保留。

## 10. 附：本轮勘察发现的三处既存问题（与本方案无关，建议顺手记账）

1. `declarative-provider.schema.json:68-76` 的 `$defs/asset` 声明
   `additionalProperties: false` 且只允许 `{path, alt}`，但 C++
   `bank_validator.cpp:55-74` 额外接受 `sourceDocument/sourcePage/autoCrop/crop`。
   **按 schema 校验会拒掉制作器的正常产物**，schema 文件落后于实现。
2. `WorkflowProgress::inputTokens/outputTokens`（`generation_workflow.hpp:23-24`）
   是旧模型路径遗留字段，规则路径恒传 0，UI 把它复用成"已处理资料份数"
   （`studio_window.cpp:1167`）。删 AI 时可一并正名。
3. `ModelVendor::anthropicProtocol`（`model_settings_dialog.cpp:45`）字段存在但
   传输层未实现 Messages API。随 `model_settings_dialog` 一起删除即可。


---

## 附：实施记录

### Phase 1（已完成）

**新增**
- `mineru_output_adapter.{hpp,cpp}`：ZIP/目录/字节三个入口，产出 `ExtractedDocument`。
- `tests/mineru_output_adapter_test.cpp`（已注册 CTest）+ `tests/fixtures/mineru-layout-fixture.json`（合成夹具）。
- `tests/mineru_regression_harness.cpp`：真卷回归入口，照 `pdf_regression_harness` 先例不注册 CTest。

**实测得出的设计修正（与原方案 §4.2 不同）**

以 **layout.json** 而非 `content_list.json` 为主入口：

1. layout.json 提供 **span 级 bbox**，`content_list.json` 只有段落级。真题的四个选项常排在同一视觉行，段落级坐标无法给每个标签独立定位，而 `optionLabelAnchors` 正是图片/公式选项裁切的唯一依据。
2. layout.json 的 `discarded_blocks` 已把页眉、页脚、页码与正文结构性分离。原方案 §4.3 计划的"已知页眉页脚文案前缀/中缀清洗"因此**不需要**——Day11 第 130 题的页脚污染在这条路径上根本不会发生。

`LayoutBlock` 降为适配器内部表示，不进入 `ExtractedDocument` 公开契约，避免规则引擎出现两套数据源分支。

**规则引擎三处修复（本地 PDF 路径同样受益，与 MinerU 无关）**

| 缺陷 | 影响 | 修复 |
|---|---|---|
| `questionPattern` 的 `\.(?!\d)` 把 `111.2016年…` 当小数否掉 | 资料分析题干几乎都以年份开头，Day11 因此丢 6 道题 | 追加例外：句点后为四位数字且紧跟年份记号时按题号处理 |
| 连续答案串 `【参考答案】CAACD DBABC…` 完全无法解析 | 采用该排版的真题**全卷答案丢失** | 新增 `contiguousAnswerRun()` + 顺序配对 |
| 答案区标题必须独占一行 | 标题与答案串同行时整段被跳过 | 新增 `isInlineAnswerSectionHeader()` |

答案串配对的安全约束（测试锁定）：整卷题数与答案数相等则顺序配对；数量不等时只在存在**唯一一段**长度相符、题号严格连续且两端断开的区间时配对；两段以上候选一律放弃。

**Day11 真卷验证**：0 题 → **19 题可用**，答案与官方 `CAACD DBABC BBCCD DABAC` 逐题一致、零错位；题号 111–130 连续无缺。第 124 题因原卷只解析出 A、B 两个选项被正确挡进硬复核。

### Phase 2（已完成）

**新增**
- `mineru_client.{hpp,cpp}`：`MineruExtractionJob` 状态机（申请链接 → PUT 上传 → 轮询 → 下载），含官方错误码到可操作提示的翻译、限流/5xx/短暂网络故障的指数退避、长任务渐进轮询、超时与取消。
- `mineru_settings_dialog.{hpp,cpp}`：单 Token 输入 + 模型版本 + 强制 OCR + **允许上传**开关（默认关闭）。
- `tests/mineru_client_test.cpp`（已注册 CTest）：纯函数协议测试 + 本地 `QTcpServer` 桩服务跑完整状态机。

**删除**
- `model_client.{hpp,cpp}`、`model_settings_dialog.{hpp,cpp}`（五厂商配置 UI）
- `studio_window` 中 `requestAiReview` / `handleAiReviewResult` / `requestAiCrop` / `handleAiCropResult` / `editModelSettings` / `ensureModelApiKeyLoaded` / `updateAiReviewAffordance` 及 `aiCropInFlight_` 布尔分派
- 死代码 `chunker.*`、`checkpoint_store.*` 及其测试
- `WorkflowProgress::inputTokens/outputTokens`（旧模型路径遗留，UI 已挪用成计数）；对应 UI 成员按实际含义改名为 `sourceCount_` / `generatedCount_` / `reviewCount_`

**保留**：`PdfExtractor` 全部、像素级下划线检测、`needsReviewQuestions` 三层信号、人工拖拽重裁（`recropReviewAsset`）。

**接线**
- `SourceMaterialGroup` 增加 `mineruZipPath` / `mineruAnswerZipPath`；非空时 `GenerationWorkflow` 用适配器替代本机提取，其余流程不变。题目文件与答案另册都会进入同一云解析队列。
- `StudioWindow::startCloudParseThenGenerate()` 在工作流之前串行完成云解析；仅对 PDF/图片启用，Token 读取失败或云解析失败都**退回本机解析**而非中断整理。运行按钮可取消云任务或本地工作流，关闭窗口也会终止当前云请求。
- 云解析中间产物存放于 `QTemporaryDir`，随整理结束自动清理。
- 每份云解析资料会在 `warnings` 里标注后端与模型版本，便于复核时重点核对，也满足 §8 的"模型版本写入诊断元数据"。

**真实 API 端到端验证**：用真 Token 跑通 申请链接 → 上传 → 轮询（进度 7/10）→ 下载 → 适配 → 19 题带答案，`backend=hybrid version=3.4.4`。

**测试状态**：与后续 master 回归项合并后，全套 28 个 CTest 全部通过；其中包含 MinerU 协议/状态机、版面适配、答案另册工作流、图片选项及 Studio 复核 UI。

### Phase 3 收尾（已完成）

- 答案另册接入云解析并写入 `mineruAnswerZipPath`；新增独立合成答案夹具，离线验证页脚过滤与答案映射。
- `ExtractedDocument::extractionBackend` 标记 MinerU 来源；只有云端提供完整、同行且有序的 A/B/C/D span 时才激活四张 `option.image`，否则维持整题截图与复核回退。
- MinerU 请求支持最多四次连续瞬时故障重试，尊重 `Retry-After`，长时间 pending 的轮询间隔从 3 秒逐级退避到 30 秒。
- 取消信号贯通“取消整理”按钮、云任务、本地工作流与关窗路径；延迟轮询/重试用任务代次隔离，不会在取消后复活旧请求。
- 真卷素材受版权限制仍不进仓库；保留 `mineru_regression_harness` 供本地 Day9/Day11 包验证，CI 使用复刻相同缺陷形态的合成 golden fixture。

### 后续体验项（不阻塞替换旧 AI 链路）

- 复杂版面自动检测与"建议云解析"提示（当前是用户在设置里显式开关，不做自动判断）
- 更多版式夹具：扫描卷、双栏卷、无答案卷、图片选项跨页、表格跨页
- 按后端统计准确率与耗时
- §10 记的三处既存问题（schema 与 validator 不一致等）
