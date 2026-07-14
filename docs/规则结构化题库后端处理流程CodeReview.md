# 规则结构化题库后端处理流程 Code Review

本文只评审题库制作器的后端处理链路，不讨论 Qt 页面布局或视觉实现。目标读者按 Java Web 服务端经验理解即可：文档提取器相当于 inbound adapter，`RuleBasedBankGenerator` 相当于无状态 domain service，`GenerationWorkflow` 是 application service，`BankValidator` 是最终 domain policy，声明式 Provider 打包器是 outbound adapter。

## 1. 最终结果和边界

规则模式不调用模型，不上传原文，也不生成 C++ Provider。最终产物仍是跨平台声明式安装包：

```text
<name>.quizpane-provider（ZIP）
  ├── manifest.json
  └── content/bank.json
```

包内没有脚本、宏、Python 字节码或动态库。`manifestVersion`、运行时 `schemaVersion` 和 `bank.schemaVersion` 都沿用当前唯一版本 2，文件名、类名和 API 不带 v1/v2 后缀。

当前答题运行时只支持单选和判断题。规则引擎可以发现多个正确选项，但会把这类题标记为待复核并默认排除，不能把多选偷偷降级为单选。多选只有在主程序作答协议、草稿、判分和解析链路完整支持后才能进入唯一 Schema。

## 2. 后端调用链

```text
StudioWindow::beginPreflight
  -> GenerationWorkflow::startRuleBased
      -> ExtractorRegistry::extract
          -> TxtMarkdownExtractor
          -> DocxExtractor
          -> PdfExtractor
              -> PDF 文字层
              -> 可选 Tesseract C++ OCR
      -> RuleBasedBankGenerator::generate
          -> 题号锚点
          -> 材料范围
          -> 选项切分
          -> 行内/文末答案匹配
          -> 解析匹配
          -> needsReview 分流
      -> GeneratedBankCandidate
  -> BankValidator
  -> writeZipArchive
  -> ProviderInstaller::inspect
  -> 从 ZIP 重新读取 bank.json
  -> DeclarativeProvider::load
```

规则模式与模型模式只共享提取器、候选 DTO、规则校验和打包边界，不存在“规则失败后偷偷调用模型”的混合路径。

## 3. 文档提取

### 3.1 TXT / Markdown

`TxtMarkdownExtractor` 先严格解码 UTF-8，失败后使用 Qt Core5Compat 的 GB18030。两种编码都失败时明确返回错误，不使用本机默认编码，因此同一文件在不同系统上的结果可复现。

### 3.2 DOCX

DOCX 是 OOXML ZIP。提取器直接使用项目已有的 C 版 `miniz` 解压 `word/document.xml`，再用 `QXmlStreamReader` 顺序读取段落、制表符和换行：

- 不要求安装 Microsoft Word；
- 不启动 Office COM；
- 不执行宏；
- 不引入 Python、Java 或 Node.js；
- ZIP 总大小限制为 128 MiB；
- 正文 XML 解压后限制为 32 MiB。

第一版读取普通段落和表格单元格中的文字。内嵌图片不会被伪装成文字；纯图片 DOCX 会明确报告没有可提取文本。

### 3.3 PDF

`PdfExtractor` 使用 Qt PDF：

1. 每页调用 `QPdfDocument::getAllText()` 读取文字层；
2. 没有文字时把该页以约 2 倍分辨率渲染；
3. 先判断页面是否真的存在可见墨迹，空白页直接跳过；
4. 扫描页交给可选的 Tesseract C++ 后端；
5. 每页之间保留换页符，规则生成器据此写入 `source.page`。

这是逐页决策，不是整本 PDF 只能选择一次 text/OCR，因此能处理文字页与扫描页混合的 PDF。

### 3.4 OCR 依赖策略

OCR 是编译期开关 `QUIZPANE_ENABLE_TESSERACT_OCR`，默认开启：

- 官方发行包内置 Tesseract、Leptonica 与 `chi_sim+eng` 语言数据；
- 关闭时仍可生成精简的定制构建，但官方发行流程不会关闭；
- 开启时只链接 Tesseract 原生 C++ API，不调用 `tesseract` 命令，不启动子进程；
- 优先初始化 `chi_sim+eng`，缺中文数据时回退 `eng`；
- 运行时优先从应用包内部定位语言数据，不依赖构建机上的安装路径。

未启用 OCR 的构建遇到真实扫描页会明确失败，不会把空文本继续送入规则解析器。

## 4. 规则解析算法

`RuleBasedBankGenerator` 是无状态纯 C++ 服务，输入相同则输出相同。

### 4.1 题号锚点

当前识别：

```text
1. 题干
1、题干
1）题干
第 1 题 题干
```

题号行到下一题号行之间形成一道题的候选块。材料标题会提前截断前一道题，防止下一组材料被追加到上一题最后一个选项。

### 4.2 选项

识别 A-F 及以下标点：

```text
A.  A．  A、  A:  A）
```

同一行多个选项会按下一个选项标记再次切分；选项后续没有新标记的行合并到当前选项。ID 统一为小写 `a` 到 `f`。

### 4.3 答案和解析

答案优先级：

1. 题块内 `答案：A`、`【答案】A`、`正确答案：A`；
2. 文末“参考答案/答案汇总/答案及解析”区域中的题号映射；
3. 没有可靠答案则进入待复核，绝不选择默认选项。

文末支持逐题解析块，也支持题块内 `【解析】`。答案字母必须实际存在于该题选项；不存在或出现多个答案都会进入待复核。

### 4.4 材料

以“材料一”“资料 1”“阅读材料”等显式标题建立材料锚点。标题到第一道子题之间是材料正文；“1-5 题”这类范围会限制 `materialId` 关联。所有材料和子题最终仍由 `BankValidator` 检查引用存在性、分类一致性、孤立材料以及正文复制。

第一版不猜测没有显式标题的任意长段落是不是材料，避免把章节说明或答案解析误识别成共享材料。

## 5. 失败语义

可用题进入 `normalQuestions`；以下情况进入 `needsReviewQuestions`：

- 题干为空；
- 少于两个完整选项；
- 没有答案；
- 答案指向不存在的选项；
- 检测到多个正确答案但运行时不支持多选。

复核页默认不勾选异常题。用户强行勾选后，最终 `BankValidator` 仍会拒绝非法结构，因此 UI 状态不能绕过领域规则。

## 6. 安全和资源上限

- 输入只读；
- DOCX 不解析宏和外部关系；
- PDF 不执行 JavaScript 或嵌入文件；
- OCR 只接收内存中的页面像素；
- 题库包只写规范化 JSON；
- 最终包会重新读取并交给安装器和声明式运行时自检；
- API Key、源文件绝对路径和 OCR 中间图像不会写入题库包。

## 7. 测试边界

- `document_extractor_test.cpp`：UTF-8 文本、真实最小 DOCX ZIP、Qt 生成的文字 PDF；
- `rule_based_generator_test.cpp`：材料范围、选项、答案全文到选项的映射、行内/文末答案、解析、缺答案、多选拒绝，并对仓库 C³ 真实样本断言 8 份材料和 38 道题全部结构化；
- `generation_workflow_test.cpp`：规则模式从磁盘文件走到 `GeneratedBankCandidate`，并断言没有 HTTP 请求；
- `material_package_e2e_test.cpp`：声明式包安装、加载、答题、解析和草稿恢复。

所有测试替身只存在于 `tests/*.cpp`。生产代码没有测试模式、固定响应或跳过校验的开关。

## 8. Review 顺序

1. `document_extractor.hpp/.cpp`：输入格式、安全上限和 OCR 降级；
2. `rule_based_generator.hpp/.cpp`：确定性解析及失败语义；
3. `GenerationWorkflow::startRuleBased()`：application service 编排；
4. `BankValidator`：最终不可绕过的质量门槛；
5. `StudioWindow::packageProvider()`：ZIP 写入、安装检查和重新加载；
6. 三层测试反推每个边界的契约。
