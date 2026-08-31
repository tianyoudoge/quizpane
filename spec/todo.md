# TODO · 可优化的地方与已存在的问题

> 基于 2026-08 对 bank-studio 两条识别链路与编辑链路的完整梳理。
> 每项给出：现象/位置 → 影响 → 建议。优先级：P0 影响正确性/用户数据；P1 明显体验问题；P2 技术债。

## A. 已知问题与缺陷

### A1. 最后一个答案区之后的题目会被静默丢弃 — P1
- **现象**：锚点扫描排除"最后一个答案区头之后的所有行"（`rule_based_generator.cpp:2253-2255`），
  设计意图是剔除末尾答案区的解析文本（"1. 某选项是第一个"）。副作用：
  卷末答案区后面如果真还有题目（如"补充练习"），**整段被当解析文本丢掉，不生成题、无警告**。
  测试 `rule_based_generator_test.cpp:1181-1214` 的注释承认了这一行为。
- **建议**：答案区之后的行若出现"题号 + ≥2 选项"的完整题结构，应作为新题目区处理
  （或至少产生 warning 提示"检测到答案区后可能存在的题目未识别"）。

### A2. JSON Schema 文件与运行时校验器不一致 — P1
- **现象**：`schemas/declarative-provider.schema.json` 的 `$defs.asset` 是
  `additionalProperties:false` 只允许 `path/alt`；但 C++ 校验器 `validAsset`
  （`bank_validator.cpp:73-92`）放行 `sourceDocument/sourcePage/autoCrop/crop`。
  运行时以 C++ 为准，但 schema 文件是对外契约，第三方工具按 schema 校验会误报。
- **建议**：把 4 个附加字段补进 schema 的 `$defs.asset`（`additionalProperties` 或显式列出）。

### A3. 多选作答链路 — 已解决（2026-08-30 核验）
- **现状**：`bank_validator.cpp:23` 接受 `multiple_choice`，规则引擎可产出多选；
  主程序已实现非互斥选项、多选草稿与 RPC 提交、集合判分、正确答案及结果展示
  （`apps/desktop-qt/src/ui/main_window.cpp:1246-1269,1310-1327,1395-1404,1503-1520`）。
- **剩余工作**：旧方案文档 `docs/题库生成器UI与集成方案.md:92`
  仍把该链路写为“待实现”，需单独更新历史文档；可再补主程序多选交互专项回归测试。

### A4. 重号题需要新版客户端 — P2
- **现象**：同号题 id/label 方案（`rule_based_generator.cpp:2724-2728`）依赖新版
  `bank_validator`，旧版客户端可能拒绝整个题库（`docs/embedded-answer-recognition.md:13-15`）。
- **建议**：在导出/交接时检测目标客户端版本，重号题库对旧版给出明确提示。

### A5. 结果 ZIP 整包驻留内存 — P2
- **现象**：MinerU 结果 ZIP 上限 512MB，`download()` 整包读进内存后一次性写盘
  （`mineru_client.cpp:504, 526-531`）。低内存机器 + 大文件可能 OOM。
- **建议**：流式下载到临时文件（边下边校验大小），或边下边解压。

### A6. OCR 失败页无二次尝试 — P2
- **现象**：Tesseract `PSM_AUTO` 空结果回退 `PSM_SPARSE_TEXT`，再空即判该页失败
  （`document_extractor.cpp:372-376`）。低质量扫描件（斜放、阴影、低 DPI）整页丢失，
  只留 warning。
- **建议**：对失败页尝试提高渲染分辨率（1.5x → 2x）或 `PSM_BLOCK_TEXT` 再试一次；
  或在 UI 复核页提示"第 N 页 OCR 失败，可改用智能解析重传该文件"。

### A7. span 级坐标均分近似 — P2
- **现象**：MinerU 一个 span 内含多个选项标签时（"A. 甲 B. 乙"合成一个 span），
  坐标按字符位置水平均分（`mineru_output_adapter.cpp:184-194`），不等同于字形级坐标。
  影响四选项独立裁图的精度（可能切到邻选项边缘）。
- **建议**：裁图时给每个选项框加水平内缩 margin（当前 ±0.018 容差已部分缓解），
  或对均分结果做像素级文字检测修正。

### A8. 智能解析题目缺原卷校对与重框入口 — 已解决（2026-08-30）
- **原现象**：MinerU 文本能切题、但行级 bbox 缺失或无法逐行匹配时，
  `captureSourceBlock()` 直接放弃预览；即使已有 `reviewOnly` 原卷图，复核页也会隐藏
  重框按钮，导致用户无法对照原卷或重新定位题目区域。
- **现状**：`captureSourcePageFallback()` 回退为原 PDF 整页校对预览；复核页允许
  `reviewOnly` 图使用“调整原卷区域”，只回写 `reviewAssets/reviewSourceImages`，不进入
  `generatedAssets` 或最终题库包。回归覆盖在 `rule_based_generator_test` 与
  `studio_review_ui_test`。

### A9. 重复页眉页脚污染题干 / 选项 — 已解决（2026-08-30）
- **原现象**：本地规则链路只在生成器后段按约 25% 的宽松阈值删除带坐标边栏；
  MinerU 主要依赖 `discarded_blocks`，云端一旦把页眉页脚误放进 `para_blocks` 就可能污染正文；
  OCR 无坐标页也没有通用处理。
- **现状**：`stripRepeatedPageFurniture()` 成为两条提取链路的共享契约：有坐标时要求
  顶/底 8% 边缘带 + 至少 40% 页面重复，无坐标时仅检查首尾行并提高到 65%；数字归一化
  支持逐页页码，清理同时移除失效锚点并保持分页。合成回归覆盖本地/MinerU/OCR 和低频正文保护；
  147 页参考题本实测得到 599 道可用题 + 1 道复核，题干/选项边栏残留为 0。

### A10. 是否含答案依赖用户手工声明 — 已解决（2026-08-30）
- **原现象**：资料行默认“含答案”，`hasAnswerKey` 在解析前由用户决定；真正无答案的资料若未手改，
  会把所有题打成缺少答案，程序也无法用已经存在的答案规则反推题库策略。
- **现状**：资料行默认 Auto；规则工作流先按含答案完整探测，`answerEvidenceDetected` 有证据时保留
  含答案结果，完全无证据时重跑为无答案。人工 Included/None 仅作覆盖。独立答案册改为
  `companionAnswerText`，题本分套后按相同套题标题分发，覆盖题号逐套重启场景。Auto 最终按
  成功绑定到具体题目的答案覆盖率决策：≤5% 自动重跑成无答案库；孤立答案区标题、少量误识别、
  未对齐记录/答案串都不能把整库锁成含答案语义，避免整库因“未识别到答案”变成 0 题可收录。
- **剩余边界**：答案册没有可匹配套题标题、且多套题号重复时，无法仅凭裸题号证明归属；仍按
  “宁缺毋滥”拒绝冲突绑定并进入复核，不做语义猜测。

### A11. 无答案随机组卷、多套题混排与字符串题号结果页 — 已解决（2026-08-30）
- **原现象**：无答案题库仍可能继承 `practice.mode=random`；同一文件识别出的多套题虽然已有
  `source.sectionId/sectionTitle`，导出后仍共用一个 catalog；结果长图又把来源题号直接转成 int，
  `1-1` 等标签会退化成 0 或错误序号。
- **现状**：`DeclarativeProvider` 对无答案库强制按 bank 原序组卷；加载单 catalog 的多套旧题库时，
  按 `document + sectionId` 在内存展开为每套一个练习入口；题号契约兼容正整数和非空字符串，
  Provider 与结果长图统一优先使用 `questionLabel`/字符串题号。旧版 v2、v3 整数题号继续有效。
- **安全边界**：发现材料跨套共享时不自动拆 catalog，避免破坏 material 外键；此类少见题库保持原入口。
- **回归**：`bank_validator_test` 覆盖字符串/整数题号；`declarative_provider_flow_test` 覆盖旧式单 catalog
  多套展开、无答案 random 收紧为 sequential、原序组卷，以及字符串题号在作答/解析结果中的透传；
  `result_image_preview_test` 覆盖长图设置后预览控件必须恢复到图片完整尺寸、结果图宽度上限，以及
  无答案选择按五题分组时题号范围与答案串不丢失。

### A12. Win7 智能解析 TLS 初始化失败 — 已解决（2026-08-31）
- **原现象**：Win7 Qt 5.15.2 包可选择智能解析，但请求 MinerU 时连续记录
  `QSslSocket::connectToHostEncrypted: TLS initialization failed`，申请上传链接前即失败。
  原因是 QtNetwork 动态加载 OpenSSL 1.1.1，而 `windeployqt` 不会自动部署第三方 TLS DLL。
- **现状**：`build-windows7-openssl.ps1` 从 OpenSSL 官方源码按 x64/x86 构建固定的 1.1.1w
  运行库并校验 SHA-256；`build-windows.ps1` 将对应 `libcrypto`/`libssl` DLL 和许可证写入绿色包。
  打包阶段从最终部署目录运行 `windows_tls_runtime_probe`，同时校验 `QSslSocket::supportsSsl()`
  与实际加载版本，缺 DLL、架构不符或 ABI 不符都会在生成 ZIP 前失败；缓存命中会显式清零
  PowerShell 的原生命令退出码，避免把上一步无关的失败码误判成 TLS 运行库准备失败。

### A13. Windows 绿色包依赖构建机运行时 — 已解决（2026-08-31）
- **原现象**：常规 Win10/11 ZIP 只有 `vc_redist.x64.exe`，没有与 EXE 同级的
  `MSVCP140.dll` / `VCRUNTIME140.dll`；Actions runner 已安装 VC++ Runtime，会掩盖干净机器
  在进程加载阶段失败。常规包也只在构建目录跑测试，没有验证解压包的 Schannel 与 OCR 闭包。
- **现状**：所有 Windows 绿色包都复制匹配工具集/架构的 MSVC CRT DLL；最终部署目录必须通过
  TLS 探针，启用 OCR 时还必须用合成扫描 PDF 通过 QtPdf + Tesseract + 模型冒烟。Windows x64
  流水线额外解压 ZIP 检查 Qt 网络/PDF/WebSockets、Schannel、MSVC CRT、平台插件和 OCR 模型，
  并确认测试专用 EXE 与无用的 `vc_redist` 安装程序没有进入产物。

### A14. 同一窗口连续整理时旧复核图片叠加内存峰值 — 已解决（2026-08-31）
- **原现象**：新任务直到 `populateReview()` 收到新结果才覆盖上一批 `generatedAssets_`、
  `reviewAssets_` 和题目树。低内存 Windows 会同时保留旧的数百张逐题校对图、新 MinerU
  ZIP/layout、页面 PNG 缓存与新校对图，可能在规则整理阶段触发“内存不足”。
- **现状**：输入与 Token 预检通过、真正创建新 workflow 之前，
  `discardPreviousGenerationForNewTask()` 先安全断开树节点指针并清空上一批完整候选、图片与
  待裁切页面。释放前后诊断事件记录系统可用物理内存、进程 Working Set/Private Usage/峰值，
  同时记录旧图片数量和字节数；`studio_review_ui_test` 锁定完整释放契约。

### A15. Win7 导出的诊断日志中文乱码 — 已解决（2026-08-31）
- **原现象**：Qt 5 `QTextStream` 在中文 Windows 上按系统本地编码写日志，反馈模块却固定按
  UTF-8 读取；应用名、阶段详情和错误原因因此在导出的反馈 JSON 中变成乱码。
- **现状**：`diagnostic_logger` 不再依赖平台默认 codec，整行显式转成 UTF-8 字节后落盘；
  初始化时若检测到旧日志不是合法 UTF-8，会先将它保留轮转为 `.1`，再创建纯 UTF-8 新日志，
  避免升级后形成 GBK/UTF-8 混合文件；
  `diagnostic_logger_test` 校验原始日志字节，`feedback_report_test` 校验中文日志经过反馈 JSON
  导出后仍可原样读取，Qt5/Qt6 与各平台共享同一编码契约。

## B. 功能缺口

### B1. CropDialog 编辑能力弱 — P1
- **已修复的可用性问题**：原卷重裁按钮已靠左固定在图片标题后，不再需要横向滚动才能看到；
  QtPdf 透明页面会先铺白，裁切窗口不再黑底黑字。
- **剩余现象**：`CropCanvas`（`studio_window.cpp:269-334`）只能**整框重画**：
  没有四角/四边句柄、没有平移缩放、没有框内微调。用户想"把裁切框下移 5 像素"
  只能凭感觉整框重拖，精度差。
- **建议**：加 8 个调整句柄 + 滚轮缩放 + 方向键微调；框选时显示当前归一化坐标数值。

### B2. 题号 / 题型 / 材料正文不可编辑 — P1
- **现象**：复核页只能改 stem/options.text/answer/solution/下划线/图片 bbox
  （矩阵见 [05](05-识别后手工编辑与导出.md) §0）。题号错了（OCR 把 44 读成 41）无法改；
  题型误判（判断题被当单选）无法改；材料正文 OCR 错字无法改（材料侧只支持下划线修正与图片重裁）。
- **建议**：
  - 题号：加只读 → 可编辑的 `questionNumber` 输入（校验唯一性，同号规则需考虑）；
  - 题型：详情面板加单选下拉（single/multiple/true_false），切换时联动答案校验；
  - 材料正文：材料节点右侧面板提供 body 编辑器（改后同样清 `underlines`）。

### B3. 无本地 MinerU 部署支持 — P2
- **现象**：仅支持 mineru.net 云服务（`mineru_client.hpp:35-36`），baseUrl 本地桩仅测试用。
  对敏感材料用户（文件上传第三方 OSS）是硬限制。
- **建议**：若 MinerU 开源版提供兼容 API，可加"自托管"配置（base URL + 无 token/自签 token）；
  协议层改动不大（四个端点同构），主要工作在私有部署的上传方式可能不是 OSS 预签名。

### B4. 复杂版面自动检测并建议云解析 — P2
- **现象**：用户在智能模式下 TXT/DOCX 仍纯本地（`studio_window.cpp:1817-1828`）；
  规则模式下遇到复杂版面（双栏、图表多）不会主动提示"此文件建议用智能解析"。
  方案文档明确列为"后续体验项"，尚未实现。
- **建议**：提取后统计"无锚点页占比 / 图形推理材料占比 / OCR 失败页占比"，
  超过阈值时在进度页提示一键切换智能解析重跑（复用云解析状态机）。

### B5. hard 复核题的 LLM 辅助 — P2
- **现象**：LLM 已从产品删除（`model_client.*` 移除），hard 题全部靠人工。
  `docs/难样本复核与统一LLM调度方案.md` 设计过统一调度，但只落了 hard/soft 信号体系，
  模型侧没有回来。
- **建议**：评估对 hard 题做可选的 LLM 二次识别（仅用于"给出候选答案 + 置信度"，
  仍走人工确认），注意与"禁止用猜测答案换取 Schema 合法"约束的边界——
  LLM 结果只能填进 review.reason 的"建议"字段，不能直接写 answer。

### B6. 跨文件/跨套题的去重 — P2
- **现象**：批量审计有"选项数离群""答案分布偏斜"（`rule_based_generator.cpp:2140-2184`），
  但没有跨题文本相似度去重：同一道题在两套题里各出现一次（常见于真题汇编）会重复收录，
  复核页"疑似重复"筛选目前只基于题号信号，没有文本级重复检测。
- **建议**：生成阶段对题干做归一化哈希 + 编辑距离粗筛，重复题打 soft 信号
  `duplicate-stem`，复核页"疑似重复"标签接管展示。

## C. 体验优化

### C1. 题干编辑后下划线全量清除 — P1
- **现象**：改题干 → 旧 `stemUnderlines` 整体清除并提示"原下划线位置可能失效"
  （`studio_window.cpp:2592-2601`）。用户只改了一个错字，所有填空下划线标记丢失，
  需要重新逐处选词标记。
- **建议**：基于编辑前后的文本 diff（公共前缀/后缀）对 offset 做平移重映射，
  只有真正被改动区间覆盖的下划线才清除。

### C2. 复核页批量操作 — P1
- **现象**：soft 题虽默认勾选，但用户想"确认全部 200 道 soft 题"只能逐题点确认
  （`confirmCurrentReviewQuestion` 一次一题 + 自动跳下一题，`studio_window.cpp:2703-2724`）。
- **建议**：树视图加"确认全部已勾选项 / 确认全部 soft"按钮（逐题跑草稿校验，
  失败的跳过并列出）。

### C3. Chunking 阶段进度无粒度 — P2
- **现象**：进度条 Extracting 20-60%、Chunking 60-90%（`studio_window.cpp:1955-1973`），
  规则引擎单份大文档（147 页样本）解析时进度条长时间停滞，用户不知道卡在哪。
- **建议**：`generate()` 按文档/套题发进度信号（`splitBookletSections` 后已知套数，
  每完成一套更新一次）。

### C4. 云解析"已提交不可撤回"提示不足 — P2
- **现象**：取消/放弃云任务后，"云端已提交任务可能仍会自行完成"（消耗额度，
  `studio_window.cpp:1694-1698`），提示语不够醒目。
- **建议**：放弃确认框中明确"该文件可能继续消耗 X 页额度，且无法取消"。

## D. 文档/代码同步（写 spec 时已发现的不一致）

| # | 不一致 | 位置 | 建议 |
|---|---|---|---|
| D1 | 扫描页渲染分辨率文档写 2x，代码是 **1.5x** | `docs/规则结构化题库后端处理流程CodeReview.md` §3 vs `document_extractor.cpp:215-218` | 更新文档 |
| D2 | 选项标点支持文档不完整（代码还支持字母+空格无标点 WPS 场景、圈码、带点数字、括号数字、全角） | CodeReview §4.2 vs `rule_based_generator.cpp:1552-1558` | 更新文档 |
| D3 | 失败清单文档是早期版本，当前已扩展为 hard/soft 两级 + 批量审计 | CodeReview §5 vs `:2041-2187` | 更新文档 |
| D4 | MinerU 方案文档写"云解析失败退回本机解析"，代码已改为"停在资料页 + 可恢复" | `docs/MinerU替换AI视觉链路方案.md` 实施记录 Phase 2 vs `studio_window.cpp:1867-1935` | 更新文档 |
| D5 | 方案文档 §4.1 原计划 content_list.json 为主入口，实际实施改为 layout.json | 方案 §4.1 vs `mineru_output_adapter.hpp:17-25` | 文档已部分自述，可加醒目勘误 |
| D6 | schema 文件 asset 定义滞后（见 A2） | `schemas/declarative-provider.schema.json:68-76` | 改 schema |
| D7 | 旧 UI/架构方案仍写“主程序多选待实现”，当前代码已实现完整作答链路 | `docs/题库生成器UI与集成方案.md:92` / `docs/架构与Provider开发导读.md:192` vs `apps/desktop-qt/src/ui/main_window.cpp:1246-1520` | 更新历史文档，不得反向把 spec 改回旧状态 |

## E. 建议的推进顺序

1. **A1**（答案区后题目丢失）+ **A2**（schema 同步）— 正确性与契约问题，改动小收益大；
2. **B1**（CropDialog 句柄）+ **C1**（下划线重映射）+ **C2**（批量确认）— 复核页高频操作体验；
3. **B2**（题号/题型/材料正文可编辑）— 需要动复核页数据结构与校验，工作量中等；
4. 主程序多选链路已实现；后续仅需补专项回归测试并清理历史文档；
5. 其余 P2 项按迭代节奏穿插。
