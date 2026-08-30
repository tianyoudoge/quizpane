# MinerU 用于复杂真题解析的适配性与 QuizPane 接入方案

> 调研日期：2026-08-17
> 样本：`数资Day9电子版合集（2027）.pdf`，10 页，约 459 KiB
> 目标范围：识别第 111-130 题的共享材料、题干、文字/图片选项、答案及它们之间的关系

## 1. 结论先行

MinerU **适合接入，但不适合直接替换 QuizPane 的启发式题库生成器**。最合适的定位是：

```text
PDF
  -> MinerU：页面理解、阅读顺序、OCR、表格、图片/坐标
  -> QuizPane：题号边界、共享材料归属、选项归属、答案映射、复核与题库 Schema
```

对本项目来说，MinerU 的价值不是“直接产出题库 JSON”，而是把目前较脆弱的 `QPdfDocument 文字层 + 页图锚点` 升级成有类型、有页码、有 bbox 的文档块。现有规则引擎仍负责真题语义，这部分是 QuizPane 已有积累，MinerU 并不理解“第 129 题四张饼图分别对应 A/B/C/D”这种业务关系。

建议采用 **本地文本层 + MinerU 版面层的证据合并架构**：默认保留当前本地解析；用户明确同意上传时，复杂 PDF 可选择 MinerU 精准 API；将来如需离线或批量处理，再接 MinerU 本地服务。不要把免登录的轻量 API 作为正式题库生产入口，因为它只返回 Markdown，本次实测中图片只剩占位符。精准 API 已成功把第 129 题拆为四张独立选项图，但第 130 题又把文字选项吞进整张 chart，因此它仍不能单独替代本地文字层和题库规则。

## 2. MinerU 能提供什么

MinerU 是文档解析层，不是考试题目 KIE（关键信息抽取）成品。官方当前提供三种可用形态：

| 形态 | 是否需要 Token | 输出 | 对 QuizPane 的适用性 |
|---|---:|---|---|
| Agent 轻量 API | 否，按 IP 限流 | 仅 Markdown | 可做快速预览/降级，不足以稳定还原图片题 |
| 精准 API | 是 | ZIP：Markdown、`content_list.json`、`middle.json`、`model.json`、图片等 | **推荐用于第一版云端接入** |
| 开源本地部署 | 否 | 完整本地输出与 HTTP API | 适合隐私/大批量，但部署成本明显更高 |

官方 API 对比页称精准接口单文件上限 200 MB、600 页，轻量接口上限 10 MB、20 页；精准接口支持 `pipeline` 与推荐的 `vlm` 模型，轻量接口使用固定轻量模型。具体参数和异步流程见 [MinerU 官方 API 文档](https://mineru.net/doc/docs/index_en/)。

完整输出对本项目最有用的字段是：

- 文本、图片、表格等块的 `type`；
- `page_idx` 与 `bbox`；
- 图片/表格的 `img_path`；
- 页尺寸以及行、span 的坐标；
- 可视化调试文件，用于人工核对版面检测。

官方输出规范明确说明，Pipeline 的行/span 和块带 bbox，图片与表格内容带 `img_path`；VLM 的 `model.json` 以页为外层数组，bbox 是 `[x0,y0,x1,y1]` 且归一化到 `[0,1]`。见 [MinerU 输出文件规范](https://opendatalab.github.io/MinerU/reference/output_files/)。这些数据正好能补齐 `ExtractedDocument` 目前只有题号、选项标签和文字行锚点的限制。

## 3. Day9 样本实测

### 3.1 测试方式

第一轮使用无需注册的 Agent 轻量接口，参数为：

```json
{
  "language": "ch",
  "page_range": "1-10",
  "enable_table": true,
  "is_ocr": false,
  "enable_formula": true
}
```

任务成功完成，排队和解析合计约 40 秒。因为轻量接口只返回 Markdown，这一轮验证的是文字阅读顺序、表格、题号与图片占位能力。

第二轮使用用户提供的 Bearer Token，分别提交 `model_version=vlm` 与 `model_version=pipeline` 两个精准任务。两项任务均成功返回 ZIP；每份 ZIP 都包含 `full.md`、`content_list.json`、`content_list_v2.json`、`model.json`、`layout.json`、原始 PDF 和 14 张图片。凭据、上传签名 URL 和结果下载 URL均未写入本文或仓库文件。

### 3.2 111-130 题结果

| 检查项 | 实测结果 | 判断 |
|---|---|---|
| 题号 111-130 | 20 个题号全部出现 | 好 |
| 普通题干 | 111-128 基本可读 | 好 |
| 文字选项 | 多数 A-D 均识别，但被压成同一行 | 可由现有 `optionsOnLine` 再拆 |
| 跨页题 115 | A、B 在第 1 页，C、D 在第 2 页；结果漏掉 C，只保留 D | 失败，需要 bbox/跨页规则 |
| 第 116-120 题共享柱线图 | 只保留 `<!-- image-->` | 轻量接口不可用 |
| 表格材料 | 两张表都转为 HTML；第 3 页食品表主体较完整，第 1 页表头出现错并/重复 | 可用但必须保留原图复核 |
| 第 129 题图片选项 | 识别出 A/B/C/D 附近四个图片占位；C 的标点丢失；无图片内容或 URL | 轻量接口不可用，精准 bbox 有潜力 |
| 第 130 题跨页图片 | 题干在第 4 页、柱图和文字选项在第 5 页；结果有占位和选项文本，但没有显式题目归属 | 需 QuizPane 跨页组装 |
| 参考答案 | 完整识别为 `DBCAC ABACD BCDBA ABADB`，正好 20 位 | 好，可映射 111-130 |
| 页眉页脚 | “微信公众号”等内容混入材料中间 | 需丢弃 header/footer 块或规则过滤 |
| 目标范围 | 第 6-9 页的其他练习也被连续输出 | 需按题号范围/章节边界裁剪 |

这份样本最能说明一件事：**Markdown 完整不等于题库结构完整**。MinerU 轻量结果可以帮助识别文字和表格，却无法把图片资产交给 QuizPane。正式接入必须拿精准 API ZIP 中的 JSON、bbox 与图片，不能只消费 `full.md`。

### 3.3 精准 API 的关键结果

| 检查项 | VLM | Pipeline | 工程结论 |
|---|---|---|---|
| 题号 111-130 | 20/20 | 20/20 | 两者都可作为题目边界证据 |
| 参考答案 | 完整 20 位 | 完整 20 位 | 可由现有答案规则映射 |
| 第 115 题跨页 C 选项 | 文本存在，但误标为下一页 `header` | 同左 | 不能无条件丢弃 header；页首疑似选项需回看上一题 |
| 第 116-120 题共享图 | 独立 `chart` + bbox + JPG | 同左 | 可稳定绑定共享材料 |
| 第 129 题 | A-D 四个文本标签 + 四个独立 image 块 | 同左 | **核心图片题验证通过** |
| 第 130 题 | 下一页独立 chart；图中包含柱图和 A-D 文字选项 | 同左 | 跨页图找到了，但文字选项需本地文字层补回 |
| 第 1 页成本表 | 表头结构正确还原为“秋粮/玉米/稻谷”三组 | 表头错并，`秋粮` 跨了 6 列且子表头为空 | 本样本优先 VLM，表格仍保留原图复核 |
| 页眉识别 | 大多标成 header，但第 5 页公众号文本与 chart 重叠 | 大多标成 header | 过滤时要结合 bbox 与业务续接，不只看类型 |

第 129 题的四个图片 bbox 分别位于第 4 页的左上、右上、左下、右下，A/B/C/D 标签也各有独立 bbox。四张导出 JPG 经视觉核验均裁切完整、没有带入相邻选项，足以直接写入四个 `option.image`。因此 MinerU 对该类“图形选项题”是实质性增强，而不是仅提供 OCR 文本。

第 130 题证明了另一面：MinerU 把第 5 页顶部从页眉到 A-D 选项整体识别成一个 chart，导出图片内容完整，但 `content_list.json` 没有四条独立选项文本。原 PDF 的文字层可以提取这些选项，所以最佳方案不是二选一，而是：

```text
MinerU：提供 chart/image/table 的类型、bbox 和裁图
本地 QPdfDocument：提供忠实文字与字符坐标
QuizPane：按页码+bbox 对齐两路结果，再执行题目规则
```

另一个重要发现是，第 115 题的 C 选项并未从精准 JSON 中消失；它因为位于下一页页眉高度，被误分类为 `header`。轻量 Markdown会丢弃该块，因而表现为漏项。适配器应保留原始块全集：如果页首 header 文本命中 `^[A-F][\.．、]`，且上一页以未完成的选项序列结束，就应将它作为跨页候选交给题目组装器，而不是直接过滤。

综合本样本，建议以 **`content_list.json` 作为稳定、简单的主入口**，以 `content_list_v2.json`/`model.json` 处理类型误判、细粒度坐标和诊断；`full.md` 只用于预览，不参与最终图片绑定。VLM 与 Pipeline 的题号和视觉块结果接近，但 VLM 的关键成本表结构明显更好，因此复杂真题默认先选 VLM，Pipeline 保留为对照和降级。

### 3.4 与当前本地链路的对照

仓库当前链路是：

1. [`document_extractor.cpp`](../apps/bank-studio/engine/src/document_extractor.cpp) 使用 `QPdfDocument` 提取文字；无文字页可选 Tesseract OCR；按需渲染页图；
2. [`rule_based_generator.cpp`](../apps/bank-studio/engine/src/rule_based_generator.cpp) 从题号、选项标签和行坐标恢复题目、材料和裁图；
3. [`generation_workflow.cpp`](../apps/bank-studio/engine/src/generation_workflow.cpp) 在后台线程串起提取与规则生成；
4. 难样本进入 `needsReviewQuestions`，必要时再交给视觉模型。

本次把原 PDF 拆成“第 1-5 页题目 + 第 10 页答案”，用本机现有的 4 份 `pdf_regression_harness` 二进制复跑，均得到“题库至少需要一道题”（即生成 0 题）。由于本机缺少 Qt6 WebSockets，当前源码未能重新编译复核，所以该结果只能视为 **现有本地构建基线**，不能据此断言最新源码一定失败。它仍说明应为这种真实 WPS PDF 建立固定回归样本。

## 4. 推荐架构

### 4.1 不要让 MinerU 直接生成 `bank.json`

建议增加一个与供应商无关的中间层：

```cpp
enum class LayoutBlockType {
    Text, Title, Image, Table, Equation, Header, Footer, PageNumber, Unknown
};

struct LayoutBlock {
    LayoutBlockType type;
    int page = 0;                 // QuizPane 内统一为 1-based
    QRectF normalizedBounds;     // 统一归一化为 0..1
    QString text;
    QString assetPath;
    double confidence = -1.0;
};

struct ExtractedDocument {
    // 保留已有字段
    QString plainText;
    QHash<int, QByteArray> pageImages;
    // 新增供应商无关的版面块
    QVector<LayoutBlock> layoutBlocks;
    QString extractionBackend;   // local-qt / mineru-pipeline / mineru-vlm
};
```

MinerU 的 Pipeline 与 VLM 坐标系不同，适配器必须在入口统一为 `QRectF(0..1)`；后续规则引擎不应知道 MinerU JSON 的原始字段名。

### 4.2 推荐的数据流

```text
用户选择 PDF
  -> 本地预检（大小、页数、是否有文字层、隐私确认）
  -> 选择解析后端
       local-qt: 现有 PdfExtractor
       mineru:   申请上传 URL -> PUT -> 轮询 -> 下载 ZIP -> 校验/解压
  -> MineruOutputAdapter
       middle/model/content_list -> LayoutBlock[]
       images/*                  -> 临时资产
       full.md                   -> 仅作预览和诊断
  -> ExamDocumentAssembler
       材料组 -> 题目边界 -> 选项 -> 答案 -> 图片绑定
  -> RuleBasedBankGenerator / Review
  -> BankValidator -> .quizpane-provider
```

### 4.3 精准 API 接入流程

对本地文件，推荐用官方批量签名上传接口，即使首版一次只传一个文件：

1. `POST /api/v4/file-urls/batch`，申请上传 URL，参数包含文件名、`model_version=vlm`、中文、表格开启；
2. `PUT` 文件到签名 URL；
3. 轮询 `GET /api/v4/extract-results/batch/{batch_id}`；
4. 完成后下载 `full_zip_url`；
5. 限制 ZIP 总大小、单文件解压大小和路径，防止 ZIP Slip/压缩炸弹；
6. 优先读取稳定的 `content_list.json`，需要精细坐标时读取 `middle.json`/`model.json`；
7. 原始 ZIP 与中间文件只放任务临时目录，完成/取消后按策略清理。

API 是异步任务，不能塞进当前同步 `DocumentExtractor::extract()` 并在 UI 线程阻塞。推荐新增 `MineruExtractionJob` 状态机，发出 `uploading/pending/running/downloading/adapting` 进度信号；ZIP 下载和 JSON 适配再放到工作线程。当前 `GenerationWorkflow` 可继续作为总编排器。

### 4.4 111-130 的组装规则

MinerU 负责告诉我们“页面上有什么、在哪里”，QuizPane 负责以下绑定：

#### 共享材料

- 识别“根据以下材料，回答问题”等组标题；
- 从标题后的第一个内容块开始，直到该组第一道题的题号块之前，归为共享材料；
- 表格/图片块既保留结构文本，也保留原卷裁图；
- 为保证显示一致，最终资产优先按 MinerU bbox 从原 PDF 高分辨率渲染图裁切，而不是盲信 MinerU 导出的压缩图片。

#### 普通题和跨页题

- 题目起点：题号正则命中且位于正文块；
- 题目终点：下一题题号、下一材料标题或答案区；
- 页末不能自动结束题目；因此 115 的 C/D 和 130 的图片/选项会自然并入下一页，直到下一边界；
- header、footer、page_number 块先过滤，不允许进入题干。

#### 图片选项（第 129 题）

- 在题目 bbox 范围内按阅读顺序收集 A-D 标签和 image 块；
- 图片中心点与选项标签的行/列位置匹配；双列布局先按 x 聚类，再按 y 排序；
- 每个标签只能绑定一张图，每张图只能使用一次；
- 绑定不满足 `A/B/C/D = 4/4` 时进入硬复核，绝不把整页图当作某个选项。

#### 题干图 + 文字选项（第 130 题）

- 题号之后、首个选项标签之前的 image 块归入 `stem.image`；
- 首个选项之后的文本按 A-D 解析；
- 允许题干图出现在下一页；
- 若图像 bbox 与选项区域重叠或跨越多题，进入复核。

#### 答案

- 识别“参考答案/答案”等标题后的连续 A-F 字母串；
- 去空格后，本样本答案为 20 位，对应 111-130；
- 只有在题号连续且答案数完全相等时才能按位置自动映射；
- 数量不等、题号缺失或出现非法字母时，整段答案进入硬复核，不能错位后静默写入。

## 5. 和现有代码的最小改造面

建议按以下边界实现，避免把网络供应商逻辑渗入规则生成器：

| 新增/修改点 | 职责 |
|---|---|
| `mineru_client.*` | Token 鉴权、签名上传、轮询、取消、超时、下载 |
| `mineru_output_adapter.*` | ZIP 校验解压；MinerU JSON -> `LayoutBlock[]` |
| `document_extractor.hpp` | 新增供应商无关 `LayoutBlock` 与 backend 元数据 |
| `exam_document_assembler.*` | 题目/材料/图片/答案的业务绑定 |
| `generation_workflow.*` | 增加远程解析阶段、进度、重试与本地回退 |
| `secret_store` | 以独立命名空间保存 MinerU Token，不写日志/配置/题库包 |
| 制作器 UI | “本地解析 / MinerU 云解析”选择、上传提示、额度/隐私提示 |

现有 `questionAnchors`、`optionLabelAnchors`、`lineAnchors` 不必立刻删除。第一版适配器可以由 MinerU 的文字块反向填充这些字段，让 `RuleBasedBankGenerator` 先复用；待回归稳定后，再逐步让它直接使用 `layoutBlocks`。

### 为什么不建议只把 `full.md` 塞给现有生成器

本次样本已经出现以下不可逆丢失：

- 115 的 C 选项缺失；
- 第 116-120 题共享图只剩占位；
- 第 129 题四张选项图只剩占位；
- 第 130 题的跨页归属没有显式结构；
- 页眉插入材料正文。

一旦只拿 Markdown，这些信息无法靠后续正则可靠恢复。**JSON+bbox 是接入 MinerU 的必要条件，而不是锦上添花。**

## 6. 模型选择和降级策略

首轮建议对同一回归集同时跑 `pipeline` 与 `vlm`，不要先写死：

- `vlm`：优先用于扫描件、复杂阅读顺序、图表与图片选项；
- `pipeline`：作为文字型 PDF 的稳定对照，重点检查文字忠实度与表格；
- 本地 `PdfExtractor`：离线、用户拒绝上传、API 失败时的保底；
- 视觉 LLM：只处理 MinerU + 规则引擎仍然不确定的局部截图，不上传整卷。

后端选择可以由预检打分：文字层完整、单栏、无图片选项时走本地；检测到扫描页、复杂双栏、多个视觉块或规则结果缺项时，建议 MinerU。不要自动上传，必须让用户看见并同意。

## 7. 免费额度、部署与许可

### 7.1 “每日免费额度”的准确理解

官方页面当前口径并不完全一致：英文 API 页写“每账号每天 2,000 页最高优先级”，中文 API 页面仍可见“每天 1,000 页最高优先级”；两者都描述为 **最高优先级解析额度，超出后降低优先级**，不是一个稳定承诺的“超出即收费”价格表。因此产品中不要硬编码“每日免费 2,000 页”。应：

- 在设置页链接官方额度页；
- API 返回额度信息时以返回值为准；
- 文案写“官方当前提供每日优先解析额度，具体以账号控制台为准”；
- 对 429、长时间 pending 做指数退避和可取消处理。

官方还公布了提交任务和查询任务的分钟级限流，但保留动态调整权，见 [API 限流说明](https://mineru.net/doc/docs/limit_en/)。

### 7.2 本地部署成本

官方当前硬件表显示：Pipeline 可纯 CPU，但最低约 16 GB RAM、20 GB 磁盘；VLM/Hybrid 本地引擎硬件要求更高，Apple Silicon 或较新 NVIDIA GPU 更合适。具体版本要求见 [MinerU 官方仓库](https://github.com/opendatalab/MinerU)。这与 QuizPane 当前“普通用户无需安装 Python/Docker”的产品边界冲突，因此不建议把本地 MinerU 打进桌面安装包；更合理的是公司/高级用户自行部署 `mineru-api`，QuizPane 连接其 HTTP 地址。

### 7.3 许可与隐私

MinerU 3.1.0 使用基于 Apache 2.0 的自定义许可。官方许可允许商业使用，但当合并口径 MAU 超过 1 亿或月收入超过 2,000 万美元时要求单独商业许可；基于 MinerU 向第三方提供在线服务时还要求显著标明使用了 MinerU。见 [MinerU Open Source License](https://github.com/opendatalab/MinerU/blob/master/LICENSE.md)。这不是法律意见，上线前仍应复核当时版本的许可和云 API 服务条款。

官方 API 文档能确认文件会通过签名 URL 上传到其 OSS/CDN，但本次未找到清晰的文件保留周期、删除接口或企业数据不训练承诺。因而云接入至少需要：

- 上传前明确告知“文档将发送至 MinerU 云服务”；
- 默认不自动上传，记住的是用户选择而非隐式同意；
- 涉密/未授权材料禁止云解析；
- Token、签名 URL、原文和 CDN URL不进入诊断日志；
- 在获得正式数据处理条款前，不对企业用户承诺本地等价隐私。

## 8. 分阶段实施建议

### Phase 0：精准 API 验证（已完成）

本轮已使用 MinerU Token 完成命令行 PoC，没有修改产品代码：

1. `vlm` 与 `pipeline` 均成功返回完整 ZIP；
2. 20 个题号、20 位答案均完整；
3. 115 的 C 选项被保留但误标 header，已有可确定修复规则；
4. 129 的四图、四标签和 bbox 完整，验证通过；
5. 130 的跨页 chart 完整，但四个文字选项没有独立结构，需要融合本地文字层；
6. 选择 `content_list.json` 为主入口，V2/model JSON 为诊断与补救入口；
7. 本样本默认推荐 VLM，Pipeline 作为对照/降级。

Phase 0 的结论是“可以进入 Phase 1”，但验收目标应改为融合输出，而不是让 MinerU 独立达到 80/80 文字选项：第 130 题已经证明两路互补是必要条件。

### Phase 1：适配器与固定回归（2-4 天）

- 实现 `LayoutBlock` 与 MinerU ZIP 适配器；
- 加入本样本的脱敏/授权固定 fixture；
- 先把块映射回现有 anchors，复用规则生成器；
- 加 111-130 的 golden JSON 和裁图尺寸/哈希测试；
- 不做 UI，只用测试工具验证。

### Phase 2：云任务工作流（3-5 天）

- 实现上传、轮询、取消、超时、断点状态；
- 接系统 SecretStore；
- 制作器增加后端选择、上传提示和错误回退；
- 下载 ZIP 后本地完成题库语义组装，不把 API 当最终真相。

### Phase 3：难样本路由（2-4 天）

- 自动检测复杂版面但只“建议”云解析；
- 将缺选项、图片绑定冲突、答案数量不符送到现有复核页；
- 只将局部裁图交给视觉 LLM 做最后校正；
- 建立按后端、文档类型统计的准确率与耗时指标。

## 9. 验收标准

Day9 样本不应只看“识别到 20 道题”，建议采用以下硬指标：

| 指标 | 通过线 |
|---|---:|
| 题号 111-130 完整率 | 20/20 |
| 每题题干非空 | 20/20 |
| A-D 选项完整率 | 80/80；图片选项也按一个选项计 |
| 答案映射 | 20/20，且与 `DBCAC ABACD BCDBA ABADB` 一致 |
| 共享材料绑定 | 4 组均只绑定到对应 5 题 |
| 第 129 题图片 | 4 张独立图片，A-D 无错位 |
| 第 130 题跨页 | 1 张题干柱图 + 4 个文字选项，归属正确 |
| 页眉页脚污染 | 0 条进入材料/题干 |
| 静默丢失 | 0；任何缺项必须进入硬复核 |

此外还要加入扫描 PDF、双栏卷、答案另册、无答案卷、题号不连续、图片选项跨页、表格跨页等 fixture。MinerU 版本和模型版本必须写入任务诊断元数据，否则将来云端模型升级后无法解释回归变化。

## 10. 最终建议

1. **值得接入**：它能明显增强复杂版面、表格、OCR 和视觉块定位，适合做 QuizPane 的第二种文档解析后端。
2. **精准 API PoC 已通过，可以开始 Phase 1**：129 的四张图片选项已完整拆分；130 的跨页图也已找到，但需与本地文字层合并。
3. **保留现有启发式算法**：题号、材料、选项、答案和复核规则仍是核心竞争力；MinerU 只替换/增强最前面的文档感知层。
4. **第一版优先云 API，不内嵌本地 MinerU**：这与现有 Qt 桌面产品的低依赖定位更一致。
5. **把失败显式化**：缺选项、跨页不确定、图片数量不符、答案错位都必须进入复核，不能为了“自动化率”产出看似合法但语义错误的题库。
