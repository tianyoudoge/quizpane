# QuizPane 题库生成器（开发中）

目标是把用户拥有版权或合法授权的 TXT、DOCX、PDF 或结构化题目转换为声明式 `.quizpane-provider` 本地题库包。声明式包只包含 JSON 和图片，不包含 DLL、dylib 或 so，因此同一个包可跨平台使用。

协议与 Agent 设计见 `docs/AI题库生成与声明式Provider方案.md`，规范数据结构见 `schemas/declarative-provider-v1.schema.json`。早期设计中的 `.quizpane-bank` 扩展名已经废弃，避免让用户理解两套安装格式。

当前只实现旧版源 JSON 校验，不会生成正式题库包；它将在声明式运行时完成后按新 Schema 重写。计划能力：

- CSV、JSON 和 QTI 导入；
- 分类、题目、选项、答案、解析和图片预览；
- Zstandard 分块压缩；
- Ed25519 发布者签名；
- 可选 XChaCha20-Poly1305 加密；
- 图形化向导与命令行批处理。

实验性构建：

```bash
cmake -S . -B build/dev -G Ninja \
  -DCMAKE_PREFIX_PATH=/path/to/Qt \
  -DQUIZPANE_BUILD_BANK_GENERATOR=ON
cmake --build build/dev --target quizpane-bank-generator
./build/dev/tools/bank-generator/quizpane-bank-generator \
  --validate tools/bank-generator/examples/example-bank.json
```

导入者必须确认其拥有题目、图片和解析的必要权利。本工具不会授予任何第三方内容许可。
