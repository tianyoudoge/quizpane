#pragma once

#include "quizpane/studio/document_extractor.hpp"
#include "quizpane/studio/review_result.hpp"

#include <QJsonArray>
#include <QList>
#include <QStringList>

#include <functional>

namespace quizpane::studio {

// 规则生成器的输出 DTO 与模型工作流保持相同的数据边界，但不依赖网络、窗口或
// 检查点。questions 可直接通过 BankValidator；needsReviewQuestions 保留
// 原始识别结果和失败原因，禁止用猜测答案换取表面上的 Schema 合法。
using RuleBasedGenerationResult = ReviewResult;

// 规则整理期间的只读进度快照。回调不参与任何识别决策，相同输入的最终结果
// 仍保持确定性；它只让工作流展示当前套题、逐题计数和图片中间产物规模。
struct RuleGenerationProgress {
    QString documentName;
    QString sectionTitle;
    int sectionIndex = 0;
    int sectionCount = 0;
    int questionIndex = 0;
    int questionCount = 0;
    int processedQuestions = 0;
    int acceptedQuestions = 0;
    int reviewQuestions = 0;
    int pageImageCount = 0;
    qint64 pageImageBytes = 0;
    int reviewAssetCount = 0;
    qint64 reviewAssetBytes = 0;
    int generatedAssetCount = 0;
    qint64 generatedAssetBytes = 0;
};

using RuleGenerationProgressCallback =
    std::function<void(const RuleGenerationProgress&)>;

// 把 TXT/Markdown、DOCX、PDF/OCR 的统一纯文本解析成声明式题库候选。
// 算法只使用题号、选项、答案、解析和材料范围等可解释规则；所有输入相同的运行
// 都得到相同输出，适合作为低成本、离线的题库构建路径。
class RuleBasedBankGenerator final {
  public:
    [[nodiscard]] RuleBasedGenerationResult
    generate(const QList<ExtractedDocument>& documents, bool hasAnswerKey = true,
             const RuleGenerationProgressCallback& progress = {}) const;
};

} // namespace quizpane::studio
