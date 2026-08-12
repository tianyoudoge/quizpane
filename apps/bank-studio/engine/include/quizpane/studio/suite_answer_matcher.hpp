#pragma once

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

namespace quizpane::studio {

// 规则生成器已经确认的题号锚点。line 是统一文本行号，number 是原卷题号；匹配器
// 只负责给这些既有锚点建立套卷级答案身份，不另做一套题目识别。
struct SuiteQuestionAnchor {
    int line = -1;
    int number = 0;
};

struct SuiteAnswerMatchResult {
    // 命中齐麟 02/03 的“题目套卷 + 末尾总答案区”或 06 的“套卷 + 套尾答案”。
    bool recognized = false;
    // 所有归属于已识别套卷的题。调用方据此禁止回退到文档级“题号 -> 答案”。
    QSet<int> suiteQuestionLines;
    // 只有套卷 key 唯一、答案数与连续题号全部校验通过时才写入。
    QHash<int, QString> answersByQuestionLine;
    // 校验失败的整套题。保留给调用方生成明确告警并进入人工复核。
    QSet<int> rejectedQuestionLines;
    // 套尾答案行不属于最后一道题的题干/选项，调用方可用它收紧题块终点。
    QSet<int> answerLines;
    QStringList warnings;
};

// Unicode NFKC、去全部空白、去科目词，但保留年份、地区/考试、A/B/C、上下半年
// 与模拟预测序号。空串表示该行不是受支持的严格整行套卷标题。
QString normalizeQilinSuiteKey(const QString& title);

SuiteAnswerMatchResult matchQilinSuiteAnswers(
    const QStringList& lines, const QList<SuiteQuestionAnchor>& questionAnchors);

// 07“一天一题学数量”的保守子集：只解析 `N-M` 范围长度与紧随 A-D 字符数
// 完全一致的答案行。带练习附加答案导致字符超长的整组不会返回任何映射。
QHash<int, QString> matchQilinDailyQuantityAnswers(
    const QStringList& lines, const QList<SuiteQuestionAnchor>& questionAnchors);

} // namespace quizpane::studio
