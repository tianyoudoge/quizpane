#pragma once

#include "quizpane/studio/document_extractor.hpp"
#include "quizpane/studio/review_result.hpp"

#include <QJsonArray>
#include <QList>
#include <QStringList>

namespace quizpane::studio {

// 规则生成器的输出 DTO 与模型工作流保持相同的数据边界，但不依赖网络、窗口或
// 检查点。questions 可直接通过 BankValidator；needsReviewQuestions 保留
// 原始识别结果和失败原因，禁止用猜测答案换取表面上的 Schema 合法。
using RuleBasedGenerationResult = ReviewResult;

// 把 TXT/Markdown、DOCX、PDF/OCR 的统一纯文本解析成声明式题库候选。
// 算法只使用题号、选项、答案、解析和材料范围等可解释规则；所有输入相同的运行
// 都得到相同输出，适合作为低成本、离线的题库构建路径。
class RuleBasedBankGenerator final {
  public:
    [[nodiscard]] RuleBasedGenerationResult
    generate(const QList<ExtractedDocument>& documents) const;
};

} // namespace quizpane::studio
