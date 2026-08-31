# 04 · 智能识别链路（MinerU）

> 代码位置：`apps/bank-studio/engine/src/mineru_client.cpp`（558 行）、
> `mineru_output_adapter.cpp`（504 行）、`generation_workflow.cpp`、
> `apps/bank-studio/src/mineru_settings_dialog.{hpp,cpp}`、
> `studio_window.cpp`（云解析编排 `:1683-1953`、Token `:431-504`、断点恢复 `:508-814`）。
> 方案文档：`docs/MinerU替换AI视觉链路方案.md`（部分实施记录已滞后，以代码为准）。

## 0. 最重要的结论（先记住）

**"智能识别"不是"模型理解题目"，而是"用 MinerU 的版面分析能力提取文本和坐标"。**

- 规则引擎（`RuleBasedBankGenerator`）是**唯一生成引擎**；MinerU 只替换"文档感知层"，
  从未有第二条"模型生成题目结构"的路径（LLM 已从产品删除，`model_client.*` 等已移除）。
- MinerU 输出经 adapter 适配成**与本地提取完全同构的 `ExtractedDocument`**
  （`\f` 分页 + 同构归一化锚点，`mineru_output_adapter.cpp:447-453`），
  之后进入同一个规则引擎——**套题切分、四种答案位置、题型识别的处理完全相同**（见 [03](03-规则识别链路.md)）。
- **二选一，无自动 fallback**：云解析失败（网络/额度/鉴权）**不静默降级**到规则模式，
  "停在资料页 + 保留任务可恢复"（`studio_window.cpp:1920-1921`）。
  ⚠️ 方案文档实施记录 Phase 2 写的是"退回本机解析"，**当前代码已改，以代码为准**。

两条路径仅有的 3 处差异：
1. 文档感知层：本地 = QPdf 文字层 + 可选 Tesseract OCR；云 = MinerU 版面 JSON（扫描件/图表/复杂版面更强）；
2. 两处 `extractionBackend.startsWith("mineru")` 分支（§4）；
3. 云端资料在 warnings 标注后端与模型版本（`generation_workflow.cpp:79-81`），复核时重点核对图表与表格。

## 1. MinerU 服务接入

### 1.1 状态机

`MineruExtractionJob`（`mineru_client.hpp:16-23`）："只负责把本地 PDF 换成一个已下载好的结果 ZIP，
不理解题库语义"。官方是**异步任务 API**（本地文件不能直接 POST），所以是状态机而非单次请求：

`Idle → RequestingUploadUrl → Uploading → Polling → Downloading → Done / Failed / Cancelled`
（`mineru_client.hpp:40-49`）

### 1.2 配置

`MineruSettings`（`mineru_client.hpp:25-37`）：
- `token`：mineru.net「API 管理」页创建，**存系统钥匙串，不写日志/配置/题库包**；
- `modelVersion = "vlm"`（"准确识别"，默认）/ `"pipeline"`（兼容/降级）；
- `language = "ch"`、`enableFormula = true`、`enableTable = true`、`isOcr = false`
  （"扫描件需要 OCR；文字型 PDF 保持 false"——**没有 OCR 全局开关**，
  "是否扫描件由 MinerU 自动判断"，`mineru_settings_dialog.cpp:160-161` 保存时强制 `isOcr=false`）；
- `baseUrl = "https://mineru.net"`（注释：便于测试指向本地桩服务；**产品仅支持云服务**，
  本地桩仅 `tests/mineru_client_test.cpp` 用 QTcpServer 模拟）。

### 1.3 四个端点

**① 申请预签名上传链接** `requestUploadUrl()`（`mineru_client.cpp:322-358`）：
`POST {base}/api/v4/file-urls/batch`，请求体（`:110-119`）：
```json
{"enable_formula": true, "enable_table": true, "language": "ch",
 "model_version": "vlm", "files": [{"name": "<文件名>", "is_ocr": true?}]}
```
`is_ocr` 仅当显式要求时才插入（协议测试锁定"未要求必须省略"）。
响应 `{"code":0, "data":{"batch_id":"...","file_urls":["<预签名PUT地址>"]}}`（`:146-169`）。
**上传完成即自动开始解析，无需再调提交接口**（`:406-407`）；发 `taskSubmitted(batchId)` 信号，
调用方持久化 batchId 支持断点恢复。首版一次只传一份文档但仍走批量接口（"它是本地文件唯一的上传方式"）。

**② PUT 上传到预签名 URL** `uploadFile()`（`:360-411`）：
- **不能带 Content-Type**，否则 OSS 签名校验失败（`:368`）；
- 强制 **HTTP/1.1**（`Http2AllowedAttribute = false`，`:370-377`）：MinerU 返回 OSS 预签名地址，
  HTTP/2 连接会偶发被边缘节点主动关闭（HTTP 0 / Connection closed），与官方 curl 示例一致；
- 传输超时 **600000ms（10 分钟）**（`:378`）；
- `QFile` 生命周期挂在 `QNetworkReply` 上（`:381-385`），否则取消上传泄漏句柄、Windows 上锁住原文件；
- 失败重试**重新走 requestUploadUrl** 拿新预签名 URL（`:397-400`）。

**③ 轮询批次状态** `poll()`（`:425-476`）：
`GET {base}/api/v4/extract-results/batch/{batchId}`。
- 状态机 `waiting-file → pending → running → converting → done / failed`（`mineru_client.hpp:72`）；
- 进度 `extract_progress.extracted_pages / total_pages`（`:198-201`）→ `progress` 信号；
- `done` 取 `full_zip_url`（`:197, 211-216`）；`done` 但无 zipUrl 视为失败；
- `extract_result` 为空数组**不是错误**（批次刚创建可能还没条目），继续轮询（`:190-194`）；
- **渐进式轮询间隔**（`schedulePoll` `:413-423`）：前 5 次 3 秒，之后按 `pollAttempts/5`
  指数退避（×2/×4/×8），上限 30 秒；
- **轮询是任务生命线**：429/5xx/连接中断只提示"网络不稳定，N 秒后继续查询（不会丢失任务）"重试，
  **绝不判任务失败**；只有 `state=failed` 才结束（`:448-461`）。

**④ 下载结果 ZIP** `download()`（`:478-538`）：
- GET `full_zip_url`（OSS 预签名），同样禁用 HTTP/2，超时 600s；
- **签名 URL 是短时的**：下载失败重试**不抱同一个 URL，回到 `poll()` 拿新签名地址**（`:506-512`）；
- 空包失败（`:518-521`）；**超过 512MB 拒绝写入**（`kMaxZipBytes`，`:23-24`，
  官方单文件限 200MB，输出含切图与原始 PDF 留余量）；整包读进内存一次性写盘。

### 1.4 认证 / 超时 / 重试 / 取消 / 恢复

- **认证** `buildMineruRequest()`（`:121-144`）：`Authorization: Bearer <token>`；
  token 为空直接返回错误不发起请求（`:123-127`）；UA `QuizPane-Question-Maker`。
- **超时**：API 请求 60s（`:142`）；上传/下载 600s。
- **重试**：`isTransientMineruFailure()`（`:70-79`）仅 408/425/429/≥500 与连接层故障（httpStatus==0）算瞬时；
  **`OperationCanceledError`（=5）绝不重试**；已有 4xx 响应视为确定性错误直接给用户。
  重试 ≤4 次（`kMaxTransientRetries` `:22`），延迟尊重 `Retry-After`（clamp 1s..60s），否则指数 1s→16s（上限 30s）。
- **错误信封双形状**（`:26-53`）：鉴权类 `{msgCode, msg, success}`（**可能带 HTTP 200**，
  `isFailureEnvelope()` `:55-66` 显式处理）、任务类 `{code, msg}`。
  官方错误码翻译表：`A0202` Token 无效 / `A0211` Token 过期 / `-60005/-60006` 文件不符合云端限制
  / `-60012` 任务不存在或已过期 / `-60018` 今日额度用尽 / `-60013` 账号无法访问 / `-60010/-60015/-60016` 云端未能完成；
  兜底"云端服务暂时不可用"——**服务端 message 只进诊断日志，绝不直接呈现给用户**（`:50-52`）。
- **取消** `cancel()`（`:540-556`）：清 reply、`++generation_` 代际号使所有排队 lambda 失效、
  `Qt::QueuedConnection` 发 `finished(false, ..., "已取消云解析")`（保证紧跟 `start()` 的取消也能被收到）。
- **断点恢复** `resume()`（`:299-320`）：只重新轮询+下载，**绝不重复上传原文件**。
  上层把 batchId/sessionId/缓存目录持久化到 QSettings（`studio_window.cpp:508-539`、`persistCloudTask()` `:755-760`），
  启动时 `offerCloudTaskResume()` 弹框"继续等待 / 不再等待"（`:769-814`）。
  注意：放弃后"云端已提交任务可能仍会自行完成"（消耗额度）。

### 1.5 Token 存储与设置对话框

- `loadMineruToken()`（`studio_window.cpp:431-444`）：从 `quizpane::SecretStore`
  （`question-maker` / `mineru-token`）**按需读**钥匙串，不在启动时读——
  macOS 对 DMG 直跑/未签名 App 每次读都可能弹授权（`:462-464`）；
- 非敏感配置进 QSettings `question-maker/mineru`；**极简 Linux 无 libsecret 时（status==4）
  仅本次运行保留 Token，绝不退化为 QSettings 明文**（`:490-497`）；
- 设置对话框（`mineru_settings_dialog.cpp:32-165`）：Token 密码框（空时"保存"禁用）、
  模型下拉两项（vlm / pipeline）、本地"今日已提交 N 个文件"计数（`:82-87`）、
  云端额度查询 `GET /api/v4/quota`（`:104-127`，展示 `user_left_quota` 高优剩余页数）；
  页脚明确标注服务"由 MinerU 免费提供"，并鸣谢出品方上海人工智能实验室。

### 1.6 隐私约束

文件经签名 URL 上传到 MinerU OSS/CDN，官方无保留周期/不训练承诺 →
上传前必须告知（UI 文案"PDF 和图片会上传至 MinerU 解析"，`mineru_settings_dialog.cpp:66-69`）；
**Token、签名 URL、原文不进诊断日志**（`mineru_client.cpp:291-295`："原文属于用户材料，凭据属于机密"，
诊断事件只记 modelVersion/isOcr/fileBytes）。

## 2. MinerU 原始输出格式与 adapter 转换

### 2.1 产物 ZIP 内容（Day11 实测 10 页 PDF）

`full.md`、`content_list.json`（段落级块：text/header/table/chart/image/page_number，带 `page_idx`）、
`layout.json`、若干切图。

### 2.2 为什么主入口是 layout.json 而不是 content_list.json

`mineru_output_adapter.hpp:17-25`：
1. **layout.json 提供 span 级 bbox**，content_list.json 只有段落级。真题"A. 甲 B. 乙"常排同一行，
   段落级坐标无法区分四个选项标签，而 `optionLabelAnchors` 是图片/公式选项裁切的唯一依据；
2. layout.json 的 `discarded_blocks` 通常已把页眉/页脚/页码与正文**结构性分离**；
   若云端偶尔误放进 `para_blocks`，共享 `stripRepeatedPageFurniture()` 再按边缘 bbox +
   跨页高频兜底，仍不依赖文案黑名单；
3. content_list.json 仅用于诊断与人工比对，不参与锚点构建。
（⚠️ 方案文档 §4.1 原计划以 content_list.json 为主入口，实施时改为 layout.json，
见方案文末"实施记录 Phase 1：实测得出的设计修正"。）

### 2.3 layout.json 结构（以 `tests/fixtures/mineru-layout-fixture.json` 为据）

```json
{
  "_backend": "...", "_version_name": "...",
  "pdf_info": [
    { "page_idx": 0,                    // 0-based
      "page_size": [595, 842],          // 页面物理尺寸（bbox 同坐标系）
      "para_blocks": [
        { "type": "text", "bbox": [x0,y0,x1,y1], "score": 0.94,
          "lines": [ { "bbox": [...], "spans": [
            { "type": "text", "content": "…", "bbox": [...] },
            { "type": "inline_equation", "content": "…", "bbox": [...] },
            { "type": "interline_equation", "content": "…", "bbox": [...] } ] } ] },
        { "type": "chart", "bbox": [...], "blocks": [ { "type": "chart_body",
              "lines": [ { "spans": [ { "type": "chart", "image_path": "chart-a.jpg" } ] } ] } ] }
      ] }
    }
  ]
}
```

### 2.4 转换流程（`mineru_output_adapter.cpp`，504 行）

入口：`adaptMineruLayout(bytes)`（`:366-455`）、`adaptMineruDirectory(dir)`（`:457-488`）、
`adaptMineruZip(zipPath)`（`:490-502`）。

**ZIP 安全读取** `readLayoutJsonFromZip()`（`:283-362`）：
- 用 `miniz` 读入内存，**不向磁盘写任何条目**（"路径校验是纵深防御而非唯一防线"，`:264-271`）；
- 候选优先级：`layout.json`(0) > `middle.json`(1) > `*_middle.json`(2)（兼容新旧版命名，`:272-281`）；
- **按不可信输入处理**：`isSafeZipEntryPath()`（`:251-262`）拒绝绝对路径/`..`/Windows 盘符/UNC；
  总包 ≤512MB、解压总量 ≤512MB、单个 layout JSON ≤64MB、页数 ≤10000（`:25-27`）。

**逐页转换**：
- 页码归一：`pageNumber = rawIndex + 1`（`:430`）；重复页保留第一份、缺失页留空白位置（均 warning，`:397-427`）；
- **页尺寸缺失 → 该页"保留文字、放弃锚点"+ 显式 warning**（`:431-436`：
  静默产出无锚点页会让题图裁切"看起来正常但永远为空"）；
- **bbox 归一化** `normalizedBounds()`（`:112-129`）：页面坐标 ÷ `page_size` → 0..1，
  与本地 PdfExtractor 同一约定——两条路径产出的锚点可被规则引擎无差别消费；
- **文本拼接** `joinSpanText()`（`:29-77`）：`text` span 直接取 content；
  `inline_equation` 包 `$…$`、`interline_equation` 包 `$$…$$`（`:36-45`）；
  span 之间**只在必要时补空格** `needsSpanSeparator()`（`:49-60`）：两端已有空白不补；
  选项标签边界必补；仅 ASCII 字母/数字相接处补——避免中文被拆成"数 量 关 系"；
- **行末题号修复** `repairTrailingQuestionNumber()`（`:86-107`）：双栏/题卡 PDF 会让 MinerU
  把题号排到视觉行末尾（"借景是……建筑1."）。只修复"≥12 字符正文 + 行末独立 1-4 位题号"这一窄形态；
  选项行、已有行首题号的行一律不碰；修复后保留**整行 bbox** 作为题号锚点
  （"span 内已经丢失题号的精确位置，不能伪造它"，`:84-85`）；
- **锚点采集** `collectAnchorsFromSpans()`（`:156-196`）：
  - 题号：span content 匹配 `^\s*(\d{1,4})\s*[、.．]`（`:158-159`）→ `questionAnchors[page]`，
    坐标用 `boundsForTextRange()`（`:142-149`）按字符位置把整 span bbox **水平细分**到题号字符段；
  - 选项标签：`(?<![A-Za-z0-9])([A-D])\s*[、.．]`（`:161-162`，只认 A-D）；
    一个 span 内多个标签（MinerU 偶尔把"A. 甲 B. 乙"合成一个 span）也按字符位置水平分段（`:184-194`）
    ——注意这是**均分近似**，不等同于字形级坐标；
  - 每行文本 + 行 bbox → `lineAnchors[page]`（`:236-239`）；
- **`discarded_blocks` 刻意不参与**（`buildPageText` 只遍历 `para_blocks`）；若云端把页眉页脚
  误归入 `para_blocks`，页面组装完成后调用 `stripRepeatedPageFurniture()`，与本地 PDF 使用
  同一组 8% 边缘带 / 40% 重复比例规则，并同步删除失效锚点；
- image/chart/table 块：span 承载 `image_path` 而非文字，"只记录版面存在性"；
  **最终裁图仍由规则引擎按 bbox 从原卷高分辨率渲染图切取，不使用 MinerU 导出的压缩图**（`:223-225`）；
- 某题的行级 bbox 缺失或无法与规范化后的文本逐行对应时，规则引擎不会丢掉校对能力：
  `captureSourcePageFallback()` 将原 PDF **整页**作为仅复核预览，并保留 `sourceDocument`、
  `sourcePage` 与全页 `autoCrop`。复核者可重新框选题目区域；该回退图不进入成品题库。
- `plainText` 各页用 `\f` 连接（`:447-449`）；`hasPageBoundaries = true`；
  `extractionBackend = "mineru-<_backend>"`（`:450-452`）；`usedOcr` 透传自请求参数；
- **全部页无文字 → error** "未能从该文档提取到任何文字"（`:442-445`）——
  不应回退空文档继续生成，应显式失败或改走本地解析。

## 3. 与规则链路的汇合及 MinerU 专属后处理

### 3.1 汇合点

`GenerationWorkflow::startRuleBased` 的 `extractOne()`（`generation_workflow.cpp:67-83`）：
`mineruZipPath` 非空 → `adaptMineruZip`，失败则该任务整体失败；成功则附加 warning
"本份资料由 MinerU 云解析（backend version）识别，请重点核对图表与表格"（`:79-81`）。
**下游不再区分来源**，除了以下两处 `extractionBackend.startsWith("mineru")` 分支。

### 3.2 MinerU 专属后处理（仅 2 处）

1. **填空横线恢复** `recoverTrailingMineruBlank()`（`rule_based_generator.cpp:1162-1190`，调用 `:1822-1825`）：
   题干含"填入…横线"提示且 MinerU 把句尾横线删掉/幻读成 ASCII 字符/引号内横线幻读成逗号时，
   替换/插入 `〔填空〕`。只处理三种窄形态（`:1176-1179`）。
2. **四张独立选项图**（`rule_based_generator.cpp:1879-1887`）：选项文字缺失（<2 个）且有 A/B/C/D
   标签坐标时，**仅当 MinerU 提供**四个标签 bbox 且同行、横向有序、原页可渲染 →
   `extractOptionImages()`（`:589-640`，依赖 `optionRowForQuestion()` `:528-567`：
   在本题题号与下一题题号之间找 y 坐标一致的完整 A-D 行，行内 ±0.018 容差）裁出 A-D 四张小图
   写入 `option.image`；任一条件不满足 → 清空并回退整题 `stemImage` + 复核。

设计约束：规则引擎**不依赖 MinerU 原始 JSON 字段名**（vendor 无关），
只用 `extractionBackend` 启用"经云端版面坐标验证的能力"（`document_extractor.hpp:49-52`）。

## 4. 端到端工作流（智能模式）

1. **资料页**：拖入文件 → 每行 `SourceRowWidget`；模式卡片"规则解析（本机）/ 智能解析（推荐）"
   （`studio_window.cpp:1200-1201`）；`selectParseMode(true)` 无 Token 时**直接带去配置**
   （"而不是把看似选中的智能模式又悄悄切回规则模式"，`:870-876`）。
2. **预检** `beginPreflight`（`:1683-1779`）：智能模式必须有 Token（`:1716-1722`）；
   每份资料默认 `AnswerPolicyHint::Auto`，不再要求用户预先声明是否含答案。
3. **云解析** `startCloudParseThenGenerate`（`:1783-1807`）+ `processNextCloudSource()`（`:1833-1953`）：
   **逐份串行**——先题目文件、后答案文件；每份新建 `MineruExtractionJob`，
   连接 `taskSubmitted`（持久化 batchId + 本地提交计数）、`stageChanged`（进度页阶段文案）、
   `progress`（总进度条 0-20% 区间）、`finished`（成功 → 写回 `mineruZipPath` → 下一份；
   失败 → 停在资料页）。有 batchId 时 `resume()` 否则 `start()`（`:1949-1952`）。
   路由：`shouldUseCloudParse()`（`:1809-1831`）——只有 PDF/图片上云
   （TXT/MD/DOCX 永不上传，"本机解析已经是无损的"）。Win7 正式包部署 Qt5Pdf 与
   OpenSSL 1.1.1w，支持相同的 MinerU 智能解析；只有显式关闭 Qt PDF 的裁剪构建恒 false。
4. **规则工作流** `startRuleBased`（`generation_workflow.cpp:25-147`）：
   QtConcurrent 工作线程提取 + `RuleBasedBankGenerator::generate`；
   Auto 先按含答案探测，成功绑定到具体题目的答案覆盖率不高于 5% 时以无答案语义重跑；
   配对答案文件作为
   `companionAnswerText` 保留到题本分套之后再分发，不在云提取阶段改变规则语义；
   进度映射（`studio_window.cpp:1955-1973`）：云解析 0-20、Extracting 20-60、Chunking 60-90、Done 100。
5. **复核页 → 打包**：与规则模式完全相同（见 [05](05-识别后手工编辑与导出.md)）。

## 5. 已知坑与限制

### 5.1 云端侧

| 限制/坑 | 依据 |
|---|---|
| **仅 mineru.net 云服务**，无本地部署支持；baseUrl 本地桩仅测试用 | `mineru_client.hpp:35-36` |
| 单文件 ≤200MB / 600 页（官方限制，-60005/-60006）；结果 ZIP 本地 512MB 上限 | `mineru_client.cpp:23-24, 38-39` |
| 免费额度超出**降级排队而非拒绝**（长 pending 靠渐进轮询 + 弱网重试兜住）；-60018 额度用尽 | `mineru_client.cpp:42-43`；docs §6 |
| 产品内**不硬编码额度数字**，文案"以账号控制台为准" | docs 方案 §6 |
| OSS 预签名 PUT 不能带 Content-Type；HTTP/2 偶发被边缘节点断连 → 强制 HTTP/1.1 | `mineru_client.cpp:368-377` |
| `full_zip_url` 短时签名：下载失败必须重新轮询拿新 URL，不能重放旧 URL | `mineru_client.cpp:506-512` |
| 鉴权失败可能带 HTTP 200（`success:false` 信封），不能只看状态码 | `mineru_client.cpp:55-66` |
| 云端模型会**静默升级** → `extractionBackend="mineru-<backend>"` + versionName 写入 warning/诊断，是回归唯一防线 | `mineru_output_adapter.cpp:450-452` |
| Win7 的 Qt 5.15.2 TLS 后端动态加载 OpenSSL；绿色包必须带架构匹配的 OpenSSL 1.1.1w DLL | `scripts/build-windows7-openssl.ps1`、`scripts/build-windows.ps1` |
| 所有 Windows 绿色包都必须从最终部署目录运行 TLS 探针：Qt 5 校验实际加载 OpenSSL 1.1.1w，Qt 6 校验并启用 Schannel 插件 | `tests/windows_tls_runtime_probe.cpp`、`scripts/build-windows.ps1` |
| 云任务可跨进程恢复，但放弃后云端任务仍可能自行完成（消耗额度） | `studio_window.cpp:1694-1698, 778-780` |

### 5.2 adapter 版面侧

| 限制 | 依据 |
|---|---|
| 双栏/题卡版式题号排到行尾 → 窄形态修复，更复杂形态不修 | `mineru_output_adapter.cpp:79-107` |
| span 级坐标 ≠ 字形级：一个 span 内多标签只能水平均分 | `:184-186` |
| 页尺寸缺失 → 该页文字保留但锚点全废 | `:431-436` |
| 图/表/公式选项：MinerU 导出切图**不用于产品**，只取 bbox 证据，像素永远来自原卷重裁 | `:223-225` |
| Day11 实测稳定缺陷模式：选项粘连（规则拆）、页脚污染（discarded_blocks + 跨页边栏兜底）、
  **漏选项（规则不可修 → 硬复核）** | docs 方案 §2.1-2.2 |
| 全卷无文字 → 整体失败（不产空文档） | `:442-445` |

### 5.3 超时/资源汇总

API 60s / 上传下载 600s；瞬时错误重试 ≤4 次；轮询 3s→30s 退避、弱网无次数上限；
ZIP 512MB / layout.json 64MB / 10000 页；结果 ZIP 整包驻留内存；
本地 PDF 页渲染像素上限 5000（`document_extractor.cpp:222-223`）。

题库制作器真正启动新任务前会先释放上一批题目树、正式附件、逐题校对图和待裁切页面，
避免“旧复核结果 + 新 ZIP/layout + 新页面缓存”在同一进程叠加峰值。诊断日志在释放前后记录
系统可用物理内存、进程 Working Set/Private Usage/峰值以及被释放图片的数量与字节数；
Windows 数据来自 Win7 可用的 `GlobalMemoryStatusEx` 与 `GetProcessMemoryInfo`。
