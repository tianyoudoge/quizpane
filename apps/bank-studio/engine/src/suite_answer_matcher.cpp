#include "quizpane/studio/suite_answer_matcher.hpp"

#include <QRegularExpression>

#include <algorithm>

namespace quizpane::studio {
namespace {

enum class SuiteSubject {
    None,
    DataAnalysis,
    Quantitative,
};

struct SuiteHeading {
    int line = -1;
    QString key;
    SuiteSubject subject = SuiteSubject::None;
};

struct AnswerEntry {
    int titleLine = -1;
    int answerLine = -1;
    QString key;
    QString values;
    bool valid = false;
};

QString compactNfkc(const QString& text) {
    QString normalized = text.normalized(QString::NormalizationForm_KC);
    static const QRegularExpression whitespace(QStringLiteral("[\\s\\x{00a0}]+"));
    normalized.remove(whitespace);
    return normalized;
}

SuiteHeading bodyHeading(const QString& text, int line) {
    const QString compact = compactNfkc(text);
    static const QRegularExpression data(QStringLiteral(
        R"(^(?:(20\d{2}(?:国考|联考|北京|山东|广东|江苏|浙江|四川)资料分析(?:[ABC]|\((?:上|下)半年\))?)|(模拟预测\d+))$)"));
    static const QRegularExpression quantitative(QStringLiteral(
        R"(^(20\d{2}(?:国考|联考|北京|山东|广东|江苏|浙江|四川)数量关系(?:[ABC]|\((?:上|下)半年\))?)$)"));
    if (data.match(compact).hasMatch())
        return {line, normalizeQilinSuiteKey(compact), SuiteSubject::DataAnalysis};
    if (quantitative.match(compact).hasMatch())
        return {line, normalizeQilinSuiteKey(compact), SuiteSubject::Quantitative};
    return {};
}

QString answerHeadingKey(const QString& text) {
    const QString compact = compactNfkc(text);
    static const QRegularExpression pattern(QStringLiteral(
        R"(^(?:20\d{2}(?:国考|联考|北京|山东|广东|江苏|浙江|四川)(?:[ABC]|\((?:上|下)半年\))?|模拟预测\d+)$)"));
    return pattern.match(compact).hasMatch() ? normalizeQilinSuiteKey(compact) : QString{};
}

bool isTotalAnswerHeader(const QString& text) {
    return compactNfkc(text) == QStringLiteral("【参考答案】");
}

bool parseCompactAnswerLine(const QString& text, QString* values) {
    const QString compact = compactNfkc(text).toUpper();
    static const QRegularExpression pattern(QStringLiteral(R"(^【参考答案】([A-D]+)$)"));
    const auto match = pattern.match(compact);
    if (!match.hasMatch())
        return false;
    if (values)
        *values = match.captured(1);
    return true;
}

bool isAnswerLikeLine(const QString& text) {
    return compactNfkc(text).startsWith(QStringLiteral("【参考答案】"));
}

QList<SuiteQuestionAnchor> anchorsInRange(
    const QList<SuiteQuestionAnchor>& anchors, int start, int end) {
    QList<SuiteQuestionAnchor> result;
    for (const SuiteQuestionAnchor& anchor : anchors)
        if (anchor.line > start && anchor.line < end)
            result.append(anchor);
    return result;
}

bool hasSafeQuestionSequence(const QList<SuiteQuestionAnchor>& anchors, int answerCount,
                             QString* failure) {
    // 这三本标准套卷只有 10/15/20 题。限制长度可防止偶然长得像套卷标题的正文
    // 触发顺序配对，也让“缺失”造成的 13 答案明确进入复核。
    if (answerCount != 10 && answerCount != 15 && answerCount != 20) {
        if (failure)
            *failure = QStringLiteral("答案数量 %1 不是标准套题长度 10/15/20").arg(answerCount);
        return false;
    }
    if (anchors.size() != answerCount) {
        if (failure)
            *failure = QStringLiteral("识别到 %1 道题，但答案有 %2 个")
                           .arg(anchors.size()).arg(answerCount);
        return false;
    }
    for (int index = 0; index < anchors.size(); ++index) {
        if (anchors.at(index).number <= 0) {
            if (failure)
                *failure = QStringLiteral("存在无效题号");
            return false;
        }
        if (index > 0 && anchors.at(index).number != anchors.at(index - 1).number + 1) {
            if (failure)
                *failure = QStringLiteral("题号 %1 后不是连续的 %2")
                               .arg(anchors.at(index - 1).number)
                               .arg(anchors.at(index).number);
            return false;
        }
    }
    return true;
}

void claimQuestions(const QList<SuiteQuestionAnchor>& anchors,
                    SuiteAnswerMatchResult* result) {
    for (const SuiteQuestionAnchor& anchor : anchors)
        result->suiteQuestionLines.insert(anchor.line);
}

void rejectQuestions(const QList<SuiteQuestionAnchor>& anchors,
                     SuiteAnswerMatchResult* result) {
    claimQuestions(anchors, result);
    for (const SuiteQuestionAnchor& anchor : anchors)
        result->rejectedQuestionLines.insert(anchor.line);
}

void bindValidatedAnswers(const QList<SuiteQuestionAnchor>& anchors,
                          const QString& values, SuiteAnswerMatchResult* result) {
    claimQuestions(anchors, result);
    for (int index = 0; index < anchors.size(); ++index)
        result->answersByQuestionLine.insert(
            anchors.at(index).line, QString(values.at(index)));
}

QString duplicateWarning(const QString& key, const QString& side) {
    return QStringLiteral("套卷 %1 的%2标题不唯一，整套答案已转人工复核").arg(key, side);
}

void matchDataAnalysis(const QStringList& lines,
                       const QList<SuiteQuestionAnchor>& anchors,
                       const QList<SuiteHeading>& allHeadings,
                       int totalAnswerLine, SuiteAnswerMatchResult* result) {
    QList<SuiteHeading> headings;
    for (const SuiteHeading& heading : allHeadings)
        if (heading.subject == SuiteSubject::DataAnalysis && heading.line < totalAnswerLine)
            headings.append(heading);
    if (headings.isEmpty())
        return;
    result->recognized = true;

    QHash<QString, QList<int>> bodyIndices;
    QList<QList<SuiteQuestionAnchor>> sectionAnchors;
    for (int index = 0; index < headings.size(); ++index) {
        bodyIndices[headings.at(index).key].append(index);
        const int end = index + 1 < headings.size() ? headings.at(index + 1).line : totalAnswerLine;
        sectionAnchors.append(anchorsInRange(anchors, headings.at(index).line, end));
        claimQuestions(sectionAnchors.last(), result);
    }

    QList<AnswerEntry> entries;
    for (int line = totalAnswerLine + 1; line < lines.size(); ++line) {
        const QString key = answerHeadingKey(lines.at(line));
        if (key.isEmpty())
            continue;
        AnswerEntry entry;
        entry.titleLine = line;
        entry.key = key;
        int end = lines.size();
        for (int cursor = line + 1; cursor < lines.size(); ++cursor)
            if (!answerHeadingKey(lines.at(cursor)).isEmpty()) {
                end = cursor;
                break;
            }
        int answerLikeCount = 0;
        for (int cursor = line + 1; cursor < end; ++cursor) {
            if (!isAnswerLikeLine(lines.at(cursor)))
                continue;
            ++answerLikeCount;
            result->answerLines.insert(cursor);
            QString values;
            if (parseCompactAnswerLine(lines.at(cursor), &values)) {
                entry.answerLine = cursor;
                entry.values = values;
                entry.valid = true;
            }
        }
        if (answerLikeCount != 1)
            entry.valid = false;
        entries.append(entry);
        line = end - 1;
    }

    QHash<QString, QList<int>> answerIndices;
    for (int index = 0; index < entries.size(); ++index)
        answerIndices[entries.at(index).key].append(index);

    for (int bodyIndex = 0; bodyIndex < headings.size(); ++bodyIndex) {
        const QString& key = headings.at(bodyIndex).key;
        const auto& questions = sectionAnchors.at(bodyIndex);
        if (bodyIndices.value(key).size() != 1) {
            rejectQuestions(questions, result);
            if (bodyIndices.value(key).first() == bodyIndex)
                result->warnings.append(duplicateWarning(key, QStringLiteral("题目区")));
            continue;
        }
        if (answerIndices.value(key).size() != 1) {
            rejectQuestions(questions, result);
            result->warnings.append(answerIndices.value(key).isEmpty()
                ? QStringLiteral("套卷 %1 没有唯一对应的答案标题，整套已转人工复核").arg(key)
                : duplicateWarning(key, QStringLiteral("答案区")));
            continue;
        }
        const AnswerEntry& answer = entries.at(answerIndices.value(key).first());
        QString failure;
        if (!answer.valid ||
            !hasSafeQuestionSequence(questions, answer.values.size(), &failure)) {
            rejectQuestions(questions, result);
            result->warnings.append(QStringLiteral("套卷 %1 无法安全配对：%2")
                .arg(key, answer.valid ? failure : QStringLiteral("答案行格式或数量不完整")));
            continue;
        }
        bindValidatedAnswers(questions, answer.values, result);
    }

    for (auto it = answerIndices.cbegin(); it != answerIndices.cend(); ++it)
        if (!bodyIndices.contains(it.key()))
            result->warnings.append(
                QStringLiteral("答案区套卷 %1 在题目区没有严格整行标题").arg(it.key()));
}

void matchQuantitative(const QStringList& lines,
                       const QList<SuiteQuestionAnchor>& anchors,
                       const QList<SuiteHeading>& allHeadings,
                       SuiteAnswerMatchResult* result) {
    QList<SuiteHeading> headings;
    for (const SuiteHeading& heading : allHeadings)
        if (heading.subject == SuiteSubject::Quantitative)
            headings.append(heading);
    if (headings.isEmpty())
        return;
    result->recognized = true;

    QHash<QString, int> keyCount;
    for (const SuiteHeading& heading : headings)
        ++keyCount[heading.key];

    QSet<QString> duplicateWarnings;
    for (int index = 0; index < headings.size(); ++index) {
        const SuiteHeading& heading = headings.at(index);
        const int end = index + 1 < headings.size() ? headings.at(index + 1).line : lines.size();
        const auto questions = anchorsInRange(anchors, heading.line, end);
        claimQuestions(questions, result);

        QList<int> answerLikeLines;
        QString values;
        bool validAnswerLine = false;
        for (int cursor = heading.line + 1; cursor < end; ++cursor) {
            if (!isAnswerLikeLine(lines.at(cursor)))
                continue;
            answerLikeLines.append(cursor);
            result->answerLines.insert(cursor);
            QString candidate;
            if (parseCompactAnswerLine(lines.at(cursor), &candidate)) {
                values = candidate;
                validAnswerLine = true;
            }
        }

        if (keyCount.value(heading.key) != 1) {
            rejectQuestions(questions, result);
            if (!duplicateWarnings.contains(heading.key)) {
                duplicateWarnings.insert(heading.key);
                result->warnings.append(duplicateWarning(heading.key, QStringLiteral("题目区")));
            }
            continue;
        }
        QString failure;
        if (answerLikeLines.size() != 1 || !validAnswerLine ||
            !hasSafeQuestionSequence(questions, values.size(), &failure)) {
            rejectQuestions(questions, result);
            const QString reason = answerLikeLines.size() != 1
                ? QStringLiteral("套尾答案行数量不是 1")
                : (!validAnswerLine ? QStringLiteral("套尾答案包含缺失标记或非 A-D 内容")
                                    : failure);
            result->warnings.append(QStringLiteral("套卷 %1 无法安全配对：%2")
                                        .arg(heading.key, reason));
            continue;
        }
        bindValidatedAnswers(questions, values, result);
    }
}

} // namespace

QString normalizeQilinSuiteKey(const QString& title) {
    QString key = compactNfkc(title);
    key.remove(QStringLiteral("资料分析"));
    key.remove(QStringLiteral("数量关系"));
    key = key.toUpper();
    static const QRegularExpression allowed(QStringLiteral(
        R"(^(?:20\d{2}(?:国考|联考|北京|山东|广东|江苏|浙江|四川)(?:[ABC]|\((?:上|下)半年\))?|模拟预测\d+)$)"));
    return allowed.match(key).hasMatch() ? key : QString{};
}

SuiteAnswerMatchResult matchQilinSuiteAnswers(
    const QStringList& lines, const QList<SuiteQuestionAnchor>& questionAnchors) {
    SuiteAnswerMatchResult result;
    QList<SuiteHeading> headings;
    QList<int> totalAnswerLines;
    for (int line = 0; line < lines.size(); ++line) {
        const SuiteHeading heading = bodyHeading(lines.at(line), line);
        if (heading.subject != SuiteSubject::None)
            headings.append(heading);
        if (isTotalAnswerHeader(lines.at(line)))
            totalAnswerLines.append(line);
    }

    // 02/03 只有一个末尾总答案区。多个总答案头更像旧有“分阶段题目+答案”格式，
    // 不启用套卷顺序配对，确保原有解析路径完全不变。
    if (totalAnswerLines.size() == 1)
        matchDataAnalysis(lines, questionAnchors, headings, totalAnswerLines.first(), &result);
    matchQuantitative(lines, questionAnchors, headings, &result);
    return result;
}

QHash<int, QString> matchQilinDailyQuantityAnswers(
    const QStringList& lines, const QList<SuiteQuestionAnchor>& questionAnchors) {
    QHash<int, QString> result;
    int answerStart = -1;
    int answerEnd = lines.size();
    const QString dailyHeader = QStringLiteral("【参考答案】一天一题学数量");
    const QString nextHeader = QStringLiteral("【参考答案】葫芦兄弟系列");
    for (int line = 0; line < lines.size(); ++line) {
        const QString compact = compactNfkc(lines.at(line));
        if (compact == dailyHeader)
            answerStart = line + 1;
        else if (answerStart >= 0 && compact == nextHeader) {
            answerEnd = line;
            break;
        }
    }
    if (answerStart < 0)
        return result;

    QHash<int, int> lineByNumber;
    for (const SuiteQuestionAnchor& anchor : questionAnchors)
        if (anchor.number >= 1 && anchor.number <= 200 &&
            !lineByNumber.contains(anchor.number))
            lineByNumber.insert(anchor.number, anchor.line);

    static const QRegularExpression group(QStringLiteral(
        R"((\d{1,3})\s*[-—~～]\s*(\d{1,3})\s+([A-Da-d]+)(?=\s|$))"));
    for (int line = answerStart; line < answerEnd; ++line) {
        auto iterator = group.globalMatch(lines.at(line));
        while (iterator.hasNext()) {
            const auto match = iterator.next();
            const int first = match.captured(1).toInt();
            const int last = match.captured(2).toInt();
            const QString values = match.captured(3).toUpper();
            if (first <= 0 || last < first || last > 200 ||
                values.size() != last - first + 1)
                continue;
            bool complete = true;
            for (int number = first; number <= last; ++number)
                if (!lineByNumber.contains(number)) {
                    complete = false;
                    break;
                }
            if (!complete)
                continue;
            for (int number = first; number <= last; ++number)
                result.insert(lineByNumber.value(number),
                              QString(values.at(number - first)));
        }
    }
    return result;
}

} // namespace quizpane::studio
