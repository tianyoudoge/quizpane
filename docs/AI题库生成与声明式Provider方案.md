# 小窗刷题：AI 题库生成与声明式 Provider 方案

## 1. 结论

用户的理解是正确的。小窗刷题应当支持两类题库，但统一使用 `.quizpane-provider` 安装包：

| 类型 | Manifest `kind` | 包内内容 | 适用场景 |
|---|---|---|---|
| 原生 Provider | `native` | DLL、dylib 或 so | 粉笔等需要登录、联网拉题、提交答案的服务 |
| 声明式 Provider | `declarative` | JSON、图片等静态资源 | 用户自己的 TXT、Word、PDF、JSON 题库 |

声明式 Provider 不包含可执行代码，不区分操作系统和 CPU 架构。一个包应同时用于 macOS、Windows、UOS 和麒麟。主程序内置声明式运行引擎，把静态题库转换成与原生 Provider 相同的分类、练习、答题、交卷和解析流程。

当前实现仍只接受原生动态库，尚未达到本方案。后续改造必须兼容现有 `manifestVersion: 1` 原生包，不能破坏已经发布的粉笔 Provider。

## 2. 为什么规范产物选择 JSON

JSON 比 YAML 更适合作为 AI 和程序之间的最终交换格式：

- JSON 没有缩进层级歧义，也不会把 `yes`、日期或前导零意外转换类型；
- Qt 可以直接解析，不增加第三方 YAML 运行库；
- JSON Schema 能进行严格、可重复的机器校验；
- Agent 可以按题输出 JSONL，最后再合并，长文档失败时不必全部重做。

YAML、Markdown、CSV 都可以作为导入阶段的中间输入，但打包前必须规范化为 `bank.json`。

## 3. 安装包结构

```text
my-bank.quizpane-provider              # 普通 ZIP，仅更换扩展名
├── manifest.json
├── content/
│   └── bank.json
└── assets/                            # 可选
    ├── q-001.webp
    └── q-002.png
```

声明式 Manifest 示例：

```json
{
  "manifestVersion": 2,
  "id": "local.xutianyou.general-knowledge",
  "name": "我的常识题库",
  "version": "1.0.0",
  "kind": "declarative",
  "runtime": {
    "format": "quizpane.bank+json",
    "schemaVersion": 1,
    "entry": "content/bank.json"
  },
  "permissions": {
    "network": false
  }
}
```

`id` 和版本号由生成器管理，不要求普通用户理解。资源路径只能是包内相对路径，禁止 `..`、绝对路径、符号链接和远程 URL。

## 4. 题库数据模型

规范文件见 `schemas/declarative-provider-v1.schema.json`。核心概念如下：

- `catalogs`：用户看到的练习入口，例如“数字推理”“常识判断”；
- `practice`：规定每次全部出题、固定数量顺序出题或固定数量随机出题；
- `questions`：题干、选项、答案、解析和可选图片；
- `source`：来源和页码，仅用于用户核对，不参与答题；
- `review`：Agent 置信度和待确认原因，不应直接展示在答题页。

本地散题并不需要源文件天然具有“套题”结构。内置引擎根据目录的 `practice` 策略动态组卷，从而与粉笔的“每次 5/15 题”体验保持一致：

- `all`：全部题目作为一套；
- `sequential`：按顺序每次取 N 题，并记录进度；
- `random`：每次随机取 N 题，可配置是否优先错题；

MVP 支持单选和判断题。多选、填空、简答和材料题应在后续协议版本加入，不能让 Agent 用非标准字段偷偷表达。多选需要主程序同步改为多选控件后再提升 Schema 版本。

## 5. AI Agent 工作流

```text
文档导入 → 本地文本/图片提取 → 分段 → 模型结构化 → 确定性校验
        → 定向修复 → 去重与组卷 → 用户预览 → 打包 → 安装试运行
```

### 5.1 文档提取

- TXT/Markdown：检测 UTF-8、GB18030 等编码后直接读取；
- DOCX：读取 OOXML 段落、表格和内嵌图片，不依赖安装 Word；
- PDF：先读取文字层；扫描件才进入 OCR，并保留页码和截图用于核对；
- 任何原文先在本地提取。除非用户明确同意，不把整份原文上传云端模型。

### 5.2 分段与模型调用

按页、标题和 token 上限切块，并给每块稳定编号。模型每次只输出 JSONL 题目记录，不直接生成 ZIP，也不负责文件路径、Manifest 或版本号。这样可以断点续跑，并限制一次错误的影响范围。

模型适配层全部内置在“题库生成器”中，用户不需要安装任何 Agent、命令行或开发环境：

1. MVP：OpenAI-compatible Chat/Responses 接口，兼容用户自备 API 和多数网关；
2. 本地模型：Ollama 的 OpenAI-compatible 接口；
3. 后续原生适配：Anthropic Messages、Gemini GenerateContent；
4. 所有分段、重试、修复、校验和打包均由生成器内部 Workflow Engine 调度。

模型能力只负责“理解和候选结构化”，所有成功判定都由本地 Harness 完成。

### 5.3 产品运行形态

题库生成器自身就是 Agent Host。用户只看到图形界面，内部模块关系如下：

```text
Qt 向导 UI
  └── Workflow Engine（任务状态机、并发、暂停和恢复）
      ├── Document Extractor（TXT/DOCX/PDF/OCR）
      ├── Model Adapter（OpenAI/兼容服务/Ollama）
      ├── Validation Harness（Schema、语义、去重）
      ├── Repair Loop（只重试失败题目）
      ├── Task Store（检查点、Token 和错误记录）
      └── Packager（生成并自检 .quizpane-provider）
```

不得把 Codex、Claude Code、Python、Node.js、Docker 或任何 Agent CLI 作为最终用户依赖。开发阶段可以使用这些工具生成测试数据，但发行版功能必须在只有小窗刷题和题库生成器的干净系统上完整运行。

## 6. Harness：如何保证产物稳定

Harness 是确定性流水线，不依赖模型自我宣称成功。

### 6.1 结构校验

- JSON 必须通过 Schema；
- ID 在包内唯一且格式合法；
- 每题至少两个选项，选项 ID 唯一；
- 单选答案恰好一个，多选答案至少两个；
- 答案必须引用真实选项；
- 目录引用的题目必须存在；
- 所有资源必须存在、位于包内且满足大小限制。

### 6.2 语义校验

- 答案不能只出现在解析中而没有对应选项；
- 题干、答案、解析不得被页眉页脚污染；
- 用规范化题干哈希和相似度做重复题检测；
- 无法确定答案、选项错位、图片缺失时标为 `needs_review`，不能静默猜测；
- 低置信度题默认不进入最终题库，除非用户确认。

### 6.3 修复循环

校验器只把具体错误及相关原文片段发回模型，最多自动修复两轮。仍失败的题进入人工复核列表，禁止无限重试。打包器只接受校验通过的 canonical JSON，因此更换模型不会改变最终质量门槛。

### 6.4 可复现性

- 保存模型名称、提示词版本、源文件 SHA-256 和每块处理状态；
- 题目 ID 由规范化内容哈希生成，不依赖模型随意命名；
- JSON 使用固定字段顺序和 UTF-8，ZIP 内时间戳归一化；
- 同一份已确认数据应生成相同的内容摘要。

## 7. 非开发者交互

主菜单使用“从文档生成题库”，不出现 Provider、Schema、Manifest、ABI 等术语。

生成向导只保留四步，模型选择放在应用级“设置”菜单，不让用户每次重复选择：

1. **选择文件**：拖入 TXT、DOCX 或 PDF；
2. **自动整理**：显示“已发现 126 题，12 题需要确认”，允许后台继续；
3. **检查问题**：只展示缺答案、选项错位、疑似重复等异常题，正常题默认折叠；
4. **完成**：填写题库名称，点击“生成并安装”。

高级设置折叠隐藏：每套题数量、顺序/随机、分类规则、OCR 语言和模型参数。默认值应能让用户一路点击“下一步”。API Key 存入系统安全凭据库，不写入题库包、日志或草稿。

## 8. 安全与合规边界

- 声明式包永远不执行脚本、宏、HTML JavaScript 或模型生成代码；
- DOCX/PDF 只作为不可信输入解析，限制页数、文件数、图片尺寸和解压体积；
- 导入时展示来源和版权确认，用户必须确认拥有使用权；
- 生成器默认不上传原文件，只发送用户明确授权的文本片段；
- 题库包不包含 API Key、原始聊天记录或本机绝对路径。

## 9. 实施顺序

1. 发布 Manifest v2、JSON Schema 和示例包；
2. 改造安装器，同时接受 `native` 与 `declarative`，保持 v1 兼容；
3. 实现内置声明式运行引擎和组卷策略；
4. 将现有 bank-generator 改为 Schema 校验、规范化和打包工具；
5. 接入 TXT/DOCX/PDF 提取与人工预览；
6. 接入 OpenAI-compatible Agent 和确定性修复 Harness；
7. 增加内置任务恢复、回归语料集和各模型适配器兼容测试。

第一阶段完成前，界面不应宣传“从文档生成题库”；协议、安装、运行、交卷和解析闭环通过后再开放入口。
