#include "quizpane/studio/rule_based_generator.hpp"
#include "quizpane/studio/option_label.hpp"

#include <QFileInfo>
#include <QHash>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

namespace quizpane::studio {
namespace {

struct SourceLine {
    QString text;
    int page = 0;
};

struct QuestionAnchor {
    int line = 0;
    int number = 0;
    QString firstStemLine;
};

struct MaterialMarker {
    int line = 0;
    int firstQuestionLine = -1;
    int firstNumber = 0;
    int lastNumber = 0;
    QString id;
};

const QRegularExpression& questionPattern() {
    static const QRegularExpression value(QStringLiteral(
        R"(^\s*(?:(?:问题|题目)\s*)?(?:第\s*)?(\d{1,4})\s*(?:题|[\.．、:：\)）])\s*(.*)$)"));
    return value;
}

const QRegularExpression& inlineAnswerPattern() {
    static const QRegularExpression value(QStringLiteral(
        R"(^\s*(?:【?\s*(?:(?:参考|标准)?答案)\s*】?|正确答案)\s*[:：]?\s*(.+?)\s*$)"));
    return value;
}

const QRegularExpression& solutionPattern() {
    static const QRegularExpression value(
        QStringLiteral(R"(^\s*(?:【?\s*(?:答案)?解析\s*】?|解答|说明)\s*[:：]?\s*(.*)$)"));
    return value;
}

bool isAnswerSectionHeader(const QString& text) {
    static const QRegularExpression pattern(
        QStringLiteral(R"(^\s*(?:答案|参考答案|答案汇总|答案及解析|参考答案及解析)\s*[:：]?\s*$)"));
    return pattern.match(text).hasMatch();
}

bool isMaterialHeader(const QString& text) {
    static const QRegularExpression pattern(QStringLiteral(
        R"(^\s*(?:(?:材料|资料|阅读材料)\s*[一二三四五六七八九十\d]*\s*[:：]?|阅读下列(?:材料|文字)|原文\s*[:：])\s*.*$)"));
    return pattern.match(text).hasMatch();
}

// 把任意选项标记归一化成小写字母 a/b/c…。支持：字母 A-I/a-i（含全角）、圈码
// ①②③④、带点数字 ⒈⒉、以及括号内/带尾括号的数字 (1)/1) 等。括号兼容全角/半角
// 与各种书写习惯：（ ( [ { 「 『 … ） ) ] } 」 』。
// 注意：用 NFC 而非 NFKC，NFKC 会把圈码 ① 分解成裸 1 而丢失选项标记。
QString normalizeOptionLabel(const QString& raw) {
    return canonicalOptionLabel(raw);
}

// 把答案文本归一化成大写字母串（如 "AC"）。支持字母、圈码、带点数字、
// 括号数字等多种写法。无法识别返回空。
QString normalizeAnswer(QString answer) {
    answer = answer.normalized(QString::NormalizationForm_C).trimmed();
    static const QRegularExpression separators(QStringLiteral("[\\s,，、;；/|+]+"));
    static const QRegularExpression closingBracket(QStringLiteral("[)）\\]}」』]"));
    answer.remove(separators);
    if (answer.isEmpty())
        return {};
    // 逐标记归一化：单字符（字母/圈码/带点数字）或括号数字片段 “(1)”。
    QString normalized;
    for (int k = 0; k < answer.size();) {
        const QChar ch = answer.at(k);
        if (ch == u'(' || ch == u'（' || ch == u'[' || ch == u'{' ||
            ch == u'「' || ch == u'『') {
            int close = answer.indexOf(closingBracket, k + 1);
            if (close > k) {
                const QString label = normalizeOptionLabel(answer.mid(k, close - k + 1));
                if (!label.isEmpty() && !normalized.contains(label.toUpper()))
                    normalized += label.toUpper();
                k = close + 1;
                continue;
            }
        }
        const QString label = normalizeOptionLabel(QString(ch));
        if (!label.isEmpty() && !normalized.contains(label.toUpper()))
            normalized += label.toUpper();
        ++k;
    }
    return normalized;
}

QString comparableText(QString value) {
    value = value.normalized(QString::NormalizationForm_KC).toLower().trimmed();
    static const QRegularExpression noise(QStringLiteral("[\\s\\p{P}\\p{S}]+"));
    value.remove(noise);
    return value;
}

QString answerFromOptionText(const QString& rawAnswer,
                             const QList<QPair<QString, QString>>& options) {
    const QString direct = normalizeAnswer(rawAnswer);
    if (!direct.isEmpty())
        return direct;
    const QString expected = comparableText(rawAnswer);
    if (expected.isEmpty())
        return {};
    QString matched;
    for (const auto& option : options) {
        const QString actual = comparableText(option.second);
        if (actual == expected ||
            (expected.size() >= 4 && (actual.contains(expected) || expected.contains(actual)))) {
            if (!matched.isEmpty())
                return {}; // 多个近似选项时拒绝猜测。
            matched = option.first.toUpper();
        }
    }
    return matched;
}

QList<SourceLine> sourceLines(const ExtractedDocument& document) {
    QList<SourceLine> result;
    int page = document.hasPageBoundaries ? 1 : 0;
    QString current;
    const QString normalized = document.plainText.normalized(QString::NormalizationForm_C);
    for (const QChar ch : normalized) {
        if (ch == u'\f') {
            result.append({current.trimmed(), page});
            current.clear();
            if (document.hasPageBoundaries)
                ++page;
        } else if (ch == u'\n' || ch == u'\r') {
            if (!current.isEmpty())
                result.append({current.trimmed(), page});
            current.clear();
        } else {
            current += ch;
        }
    }
    if (!current.isEmpty())
        result.append({current.trimmed(), page});
    return result;
}

// 一个答案区头之后的作用域终点：遇到下一个题号锚点行或下一个材料头行就
// 停止。这样阶段分组的文件里，阶段二的题目不会被前一阶段的答案区吞掉，而
// 是被重新识别为题目。两个集合都按行号升序传入。
int answerSectionEnd(const QList<SourceLine>& lines, int sectionStart,
                     const QList<QuestionAnchor>& anchors,
                     const QList<MaterialMarker>& materials) {
    const int count = lines.size();
    int end = count;
    for (const auto& anchor : anchors) {
        if (anchor.line > sectionStart && anchor.line < end) {
            end = anchor.line;
            break;
        }
    }
    for (const auto& material : materials) {
        if (material.line > sectionStart && material.line < end)
            end = material.line;
    }
    return end;
}

QHash<int, QString> globalAnswers(const QList<SourceLine>& lines, int sectionStart, int sectionEnd) {
    QHash<int, QString> answers;
    if (sectionStart < 0)
        return answers;
    const int limit = sectionEnd >= 0 ? sectionEnd : lines.size();
    // 表格化答案区：形如
    //   题号 | 1 | 2 | 3
    //   答案 | A | B | C
    // 或 markdown | 1 | 2 | 3 | 与 | A | B | C |。按列对齐配对。
    static const QRegularExpression cell(QStringLiteral("[^|｜\\s,，]+"));
    // 下面四个临时正则原本写在循环里逐行构造，每行都重新编译一次模式。提到
    // static const 后只编译一次，整份资料扫描的热路径显著变快。
    static const QRegularExpression pipeMarker(QStringLiteral("[|｜]"));
    static const QRegularExpression numberHeader(QStringLiteral("题号|题"));
    static const QRegularExpression answerHeader(QStringLiteral("答案|答"));
    for (int index = sectionStart + 1; index < limit && index < lines.size(); ++index) {
        const QString a = lines.at(index).text.trimmed();
        if (a.isEmpty() || !a.contains(pipeMarker))
            continue;
        if (!a.contains(numberHeader))
            continue;
        // 下一非空行作为答案行。
        int answerLine = index + 1;
        while (answerLine < limit && answerLine < lines.size() &&
               lines.at(answerLine).text.trimmed().isEmpty())
            ++answerLine;
        if (answerLine >= lines.size())
            break;
        const QString b = lines.at(answerLine).text.trimmed();
        if (!b.contains(pipeMarker) || !b.contains(answerHeader))
            continue;
        // 抽取两行的非分隔单元格，按列配对（题号行第一格“题号”是表头，跳过）。
        auto cells = [](const QString& row) {
            QList<QString> out;
            auto it = cell.globalMatch(row);
            while (it.hasNext())
                out.append(it.next().captured(0));
            return out;
        };
        const QList<QString> numCells = cells(a);
        const QList<QString> ansCells = cells(b);
        // 去掉表头后逐列配对。
        const int offset = 1;
        for (int col = offset; col < numCells.size() && col < ansCells.size(); ++col) {
            bool ok = false;
            const int number = numCells.at(col).toInt(&ok);
            if (!ok || number <= 0)
                continue;
            const QString answer = normalizeAnswer(ansCells.at(col));
            if (!answer.isEmpty() && !answers.contains(number))
                answers.insert(number, answer);
        }
        index = answerLine; // 跳过已消费的答案行
    }
    // 答案 token：字母 A-F（大小写）、圈码 ①-⑩、或带括号/带点的数字。捕获后
    // 统一经 normalizeAnswer 归一化，兼容选项是圈码或数字括号的答案区。
    const QString answerToken(QStringLiteral(
        R"([A-Fa-f]{1,6}|[①②③④⑤⑥⑦⑧⑨⑩]{1,6}|[（(\[{「『]\s*[1-9]\s*[)）\]}」』]|[1-9]\s*[)）\]」』])"));
    const QString pairPattern =
        QStringLiteral(R"((?:^|[\s,，;；])(?:第\s*)?(\d{1,4})\s*(?:题|[\.．、:：\)）-])?\s*(?:【?答案】?\s*[:：]?)?\s*()") +
        answerToken +
        QStringLiteral(R"()(?=$|[\s,，;；]))");
    static const QRegularExpression pair(pairPattern);
    static const QRegularExpression range(
        QStringLiteral(R"((\d{1,4})\s*(?:[-—~～]|至|到)\s*(\d{1,4})\s*[:：]?\s*([A-Fa-f①②③④⑤⑥⑦⑧⑨⑩]+))"));
    static const QRegularExpression answerRecord(
        QStringLiteral(R"(^\s*(?:第\s*)?(\d{1,4})\s*(?:题|[\.．、:：\)）])\s*)"));
    const QString narrativePattern =
        QStringLiteral(R"((?:故\s*)?(?:(?:正确|参考|标准)\s*)?答案\s*(?:为|是|[:：])\s*()") +
        answerToken +
        QStringLiteral(R"())");
    static const QRegularExpression narrativeAnswer(narrativePattern);
    int currentNumber = 0;
    const int lineLimit = sectionEnd >= 0 ? sectionEnd : lines.size();
    for (int index = sectionStart + 1; index < lineLimit && index < lines.size(); ++index) {
        const QString line = lines.at(index).text;
        const auto record = answerRecord.match(line);
        if (record.hasMatch()) currentNumber = record.captured(1).toInt();
        const auto rangeMatch = range.match(line);
        if (rangeMatch.hasMatch()) {
            const int first = rangeMatch.captured(1).toInt();
            const int last = rangeMatch.captured(2).toInt();
            const QString rawValues = rangeMatch.captured(3);
            // 逐字符归一化（兼容字母与圈码答案），长度与题号数相同时才展开。
            QString values;
            for (const QChar& ch : rawValues) {
                const QString one = normalizeAnswer(QString(ch));
                if (!one.isEmpty())
                    values += one;
            }
            if (last >= first && last - first + 1 == values.size()) {
                for (int number = first; number <= last; ++number)
                    if (!answers.contains(number))
                        answers.insert(number, QString(values.at(number - first)));
            }
        }
        auto matches = pair.globalMatch(line);
        while (matches.hasNext()) {
            const auto match = matches.next();
            const int number = match.captured(1).toInt();
            const QString answer = normalizeAnswer(match.captured(2));
            if (number > 0 && !answer.isEmpty() && !answers.contains(number))
                answers.insert(number, answer);
        }
        // 很多真题解析不是“1. A”式答案汇总，而是在题目解析末尾写“故正确
        // 答案为 C”。用最近一个题号归属这条结论，兼容题目文件与答案文件分离。
        const auto narrative = narrativeAnswer.match(line);
        if (currentNumber > 0 && narrative.hasMatch() && !answers.contains(currentNumber)) {
            const QString answer = normalizeAnswer(narrative.captured(1));
            if (!answer.isEmpty()) answers.insert(currentNumber, answer);
        }
    }
    return answers;
}

QHash<int, QString> globalSolutions(const QList<SourceLine>& lines, int sectionStart, int sectionEnd) {
    QHash<int, QString> solutions;
    if (sectionStart < 0)
        return solutions;
    static const QRegularExpression record(QStringLiteral(
        R"(^\s*(?:第\s*)?(\d{1,4})\s*(?:题|[\.．、\)）])\s*(?:【?答案】?\s*[:：]?)?\s*[A-Fa-f]{1,6}\s*(.*)$)"));
    int currentNumber = 0;
    bool collecting = false;
    const int limit = sectionEnd >= 0 ? sectionEnd : lines.size();
    for (int index = sectionStart + 1; index < limit && index < lines.size(); ++index) {
        const QString line = lines.at(index).text.trimmed();
        const auto recordMatch = record.match(line);
        if (recordMatch.hasMatch()) {
            currentNumber = recordMatch.captured(1).toInt();
            collecting = false;
            QString tail = recordMatch.captured(2).trimmed();
            const auto solutionMatch = solutionPattern().match(tail);
            if (solutionMatch.hasMatch()) {
                tail = solutionMatch.captured(1).trimmed();
                collecting = true;
            } else {
                const int marker = tail.indexOf(
                    QRegularExpression(QStringLiteral("(?:【?解析】?|答案解析)\\s*[:：]?")));
                if (marker >= 0) {
                    tail = tail.mid(marker).replace(QRegularExpression(QStringLiteral(
                                                        "^(?:【?解析】?|答案解析)\\s*[:：]?\\s*")),
                                                    {});
                    collecting = true;
                }
            }
            if (collecting && !tail.isEmpty())
                solutions.insert(currentNumber, tail);
            continue;
        }
        const auto solutionMatch = solutionPattern().match(line);
        if (solutionMatch.hasMatch() && currentNumber > 0) {
            collecting = true;
            const QString first = solutionMatch.captured(1).trimmed();
            if (!first.isEmpty())
                solutions.insert(currentNumber, first);
            continue;
        }
        if (collecting && currentNumber > 0 && !line.isEmpty()) {
            QString value = solutions.value(currentNumber);
            if (!value.isEmpty())
                value += QChar('\n');
            solutions.insert(currentNumber, value + line);
        }
    }
    return solutions;
}

// 切分一行内的选项。返回选项列表（label 已归一化为 a/b/c…）与首个选项 marker
// 之前的题干前缀。支持的 marker 形式：字母 `A.`、圈码 `①`、带点数字 `⒈`、
// 括号数字 `(1)` 与尾括号数字 `1)`。
// - 仅 1 个 marker 且 marker 不在行首：拒绝（避免把含“B.”的英文题干误切）。
// - ≥2 个 marker：即使 marker 前有题干文字也切分，前缀文本经 *prefix 回传给
//   parseQuestion 合并进题干（覆盖“1. 题干 A.甲 B.乙 C.丙 D.丁”单行写法）。
// - 1 个 marker 且在行首：原纯选项行行为。
QList<QPair<QString, QString>> optionsOnLine(const QString& line, QString* prefix = nullptr) {
    // 各 marker 捕获其 label 原文，后续统一归一化。捕获组按出现顺序：
    // 1=字母, 2=圈码, 3=括号数字 (1), 4=尾括号数字 1)。
    // 括号兼容全角/半角与各种书写习惯：（ ( [ { 「 『 … ） ) ] } 」 』。
    // 选项字母大小写都接受（A-F / a-f）。
    static const QRegularExpression marker(QStringLiteral(
        R"((?<![A-Za-z0-9])([A-Fa-f])\s*[\.．、:：\)）]\s*)"            // 字母 + 分隔（分隔必需）
        R"(|([①②③④⑤⑥⑦⑧⑨⑩])\s*[:：\.．、\)）\]」』]?\s*)"          // 圈码 + 可选分隔
        R"(|([①②③④⑤⑥⑦⑧⑨⑩])\s+)"                                  // 圈码 + 空格分隔
        R"(|[（(\[{「『]\s*([1-9])\s*[)）\]}」』]\s*)"                  // (1) 形式（各种括号）
        R"(|(?<![A-Za-z0-9])([1-9])\s*[)）\]」』]\s*)"));               // 1) 形式
    QList<QRegularExpressionMatch> markers;
    auto iterator = marker.globalMatch(line);
    while (iterator.hasNext())
        markers.append(iterator.next());
    if (markers.isEmpty())
        return {};
    const QString lead = line.left(markers.first().capturedStart());
    const bool hasLeadText = !lead.trimmed().isEmpty();
    // 单 marker 且前面有文字 → 视为题干，不当选项切。
    if (markers.size() == 1 && hasLeadText)
        return {};
    // ≥2 marker → 切分；若有 lead 文本则作为题干前缀回传。
    if (prefix && hasLeadText)
        *prefix = lead.trimmed();
    QList<QPair<QString, QString>> result;
    for (int index = 0; index < markers.size(); ++index) {
        const auto& m = markers.at(index);
        // 从匹配里取出非空捕获组作为 label 原文，再归一化。
        QString rawLabel;
        for (int g = 1; g <= m.lastCapturedIndex(); ++g)
            if (!m.captured(g).isEmpty()) {
                rawLabel = m.captured(g);
                break;
            }
        const QString label = normalizeOptionLabel(rawLabel);
        if (label.isEmpty())
            continue;
        const int start = m.capturedEnd();
        const int end =
            index + 1 < markers.size() ? markers.at(index + 1).capturedStart() : line.size();
        const QString text = line.mid(start, end - start).trimmed();
        if (!text.isEmpty())
            result.append({label, text});
    }
    return result;
}

// 题干/选项行末尾带的答案抽取。返回归一化后的答案字母串，空表示未识别。
// 支持两种尾随形式：
//   1) “答案：A” / “【答案】AB” 这类带答案词的；
//   2) “（A）” / “(A)” / “（AB）” 这类题干末尾直接括号写答案的（选择题写法）。
// 优先级低于以答案词开头的整行答案。
QString trailingAnswer(const QString& line) {
    static const QRegularExpression wordAnswer(QStringLiteral(
        R"((?:【?\s*(?:参考|标准)?答案\s*】?|正确答案)\s*[:：]\s*([A-Fa-f]{1,6})\s*$)"));
    auto match = wordAnswer.match(line);
    if (match.hasMatch())
        return normalizeAnswer(match.captured(1));
    // 题干末尾括号答案：（A） 「AB」 [A] {A}，括号兼容各种书写习惯。要求括号前
    // 有题干文字（start!=0），避免把整行选项“(A) ...”误判为答案。
    static const QRegularExpression bracketAnswer(QStringLiteral(
        R"([（(\[{「『]\s*([A-Fa-f]{1,6})\s*[)）\]}」』]\s*$)"));
    match = bracketAnswer.match(line);
    if (match.hasMatch()) {
        const int start = match.capturedStart();
        if (start == 0)
            return {};
        return normalizeAnswer(match.captured(1));
    }
    return {};
}

QString materialIdForQuestion(const QList<MaterialMarker>& materials, int questionLine,
                              int questionNumber) {
    QString id;
    for (const auto& material : materials) {
        if (material.line >= questionLine)
            break;
        if (material.firstQuestionLine > questionLine)
            continue;
        if (material.firstNumber > 0 &&
            (questionNumber < material.firstNumber || questionNumber > material.lastNumber))
            continue;
        id = material.id;
    }
    return id;
}

int nextMaterialLine(const QList<MaterialMarker>& materials, int afterLine, int fallback) {
    for (const auto& material : materials)
        if (material.line > afterLine && material.line < fallback)
            return material.line;
    return fallback;
}

QJsonObject parseQuestion(const ExtractedDocument& document, const QList<SourceLine>& lines,
                          const QuestionAnchor& anchor, int blockEnd, const QString& stableId,
                          const QString& materialId, const QHash<int, QString>& answerKey,
                          const QHash<int, QString>& solutionKey, QString* reviewReason) {
    QStringList stemLines;
    QList<QPair<QString, QString>> options;
    QString rawAnswer;
    QStringList solutionLines;
    bool inSolution = false;
    bool afterOptions = false;
    QString firstPrefix;
    if (!anchor.firstStemLine.isEmpty()) {
        // 题号行可能同行带选项：1. 题干 A.甲 B.乙…。先抽选项，剩余文本作为题干。
        const auto firstOptions = optionsOnLine(anchor.firstStemLine, &firstPrefix);
        if (!firstOptions.isEmpty())
            options += firstOptions;
        const QString tailAnswer = trailingAnswer(anchor.firstStemLine);
        if (!tailAnswer.isEmpty())
            rawAnswer = tailAnswer;
        // 题干前缀：优先用切选项后剩的前缀，否则用整行（移除尾随答案）。
        if (!firstPrefix.isEmpty())
            stemLines.append(firstPrefix);
        else {
            const auto answerMatch = inlineAnswerPattern().match(anchor.firstStemLine);
            if (answerMatch.hasMatch()) {
                rawAnswer = answerMatch.captured(1).trimmed();
            } else {
                QString stem = anchor.firstStemLine;
                static const QRegularExpression stripTrailing(QStringLiteral(
                    R"((?:【?\s*(?:参考|标准)?答案\s*】?|正确答案)\s*[:：]\s*[A-Fa-f]{1,6}\s*$)"));
                stem.remove(stripTrailing);
                if (!stem.trimmed().isEmpty())
                    stemLines.append(stem.trimmed());
            }
        }
    }

    for (int index = anchor.line + 1; index < blockEnd; ++index) {
        const QString line = lines.at(index).text.trimmed();
        if (line.isEmpty())
            continue;
        const auto answerMatch = inlineAnswerPattern().match(line);
        if (answerMatch.hasMatch()) {
            rawAnswer = answerMatch.captured(1).trimmed();
            inSolution = false;
            continue;
        }
        const auto solutionMatch = solutionPattern().match(line);
        if (solutionMatch.hasMatch()) {
            inSolution = true;
            const QString first = solutionMatch.captured(1).trimmed();
            if (!first.isEmpty())
                solutionLines.append(first);
            continue;
        }
        if (inSolution) {
            solutionLines.append(line);
            continue;
        }
        // 行尾可能带尾随答案（“答案：A”或题干末尾“（A）”括号答案）。先剥掉再
        // 切选项，避免把答案文本粘到最后一个选项文本末尾。
        static const QRegularExpression trailingAnswerCut(QStringLiteral(
            R"(\s*(?:【?\s*(?:参考|标准)?答案\s*】?|正确答案)\s*[:：]\s*[A-Fa-f]{1,6}\s*$"
            R"(|[（(]\s*[A-Fa-f]{1,6}\s*[)）]\s*$))"));
        QString lineForOptions = line;
        const auto cutMatch = trailingAnswerCut.match(lineForOptions);
        if (cutMatch.hasMatch()) {
            lineForOptions = lineForOptions.left(cutMatch.capturedStart()).trimmed();
            const QString tailAnswer = trailingAnswer(line);
            if (rawAnswer.isEmpty() && !tailAnswer.isEmpty())
                rawAnswer = tailAnswer;
        }
        const auto parsedOptions = optionsOnLine(lineForOptions, nullptr);
        if (!parsedOptions.isEmpty()) {
            options += parsedOptions;
            afterOptions = true;
        } else if (afterOptions && !options.isEmpty()) {
            // 选项已出现后的续行：仍可能含尾随答案，已剥过；归入最后选项文本。
            options.last().second += QStringLiteral("\n") + lineForOptions;
        } else {
            stemLines.append(lineForOptions.isEmpty() ? line : lineForOptions);
        }
    }
    QString answer = answerFromOptionText(rawAnswer, options);
    if (answer.isEmpty())
        answer = answerKey.value(anchor.number);
    if (solutionLines.isEmpty() && solutionKey.contains(anchor.number))
        solutionLines.append(solutionKey.value(anchor.number));

    QJsonArray jsonOptions;
    QSet<QString> optionIds;
    for (const auto& option : options) {
        if (optionIds.contains(option.first))
            continue;
        optionIds.insert(option.first);
        jsonOptions.append(QJsonObject{{"id", option.first}, {"text", option.second}});
    }
    QJsonArray answerIds;
    for (const QChar choice : answer) {
        const QString id(choice.toLower());
        if (optionIds.contains(id))
            answerIds.append(id);
    }

    QStringList reasons;
    if (stemLines.join(QChar('\n')).trimmed().isEmpty())
        reasons.append(QStringLiteral("缺少题干"));
    if (jsonOptions.size() < 2)
        reasons.append(QStringLiteral("未识别到至少两个完整选项"));
    if (answer.isEmpty())
        reasons.append(rawAnswer.isEmpty() ? QStringLiteral("未识别到答案")
                                           : QStringLiteral("答案文本无法唯一匹配选项"));
    else if (answerIds.size() != answer.size())
        reasons.append(QStringLiteral("答案引用了不存在的选项"));
    if (answerIds.size() > 1)
        reasons.append(QStringLiteral("识别为多选题，但当前答题运行时暂不支持多选"));

    QString type = QStringLiteral("single_choice");
    if (jsonOptions.size() == 2) {
        const QString first = jsonOptions.at(0).toObject().value("text").toString();
        const QString second = jsonOptions.at(1).toObject().value("text").toString();
        if (((first.contains(QStringLiteral("正确")) && second.contains(QStringLiteral("错误"))) ||
             (first.contains(QStringLiteral("错误")) && second.contains(QStringLiteral("正确")))) ||
            ((first == QStringLiteral("对") && second == QStringLiteral("错")) ||
             (first == QStringLiteral("错") && second == QStringLiteral("对"))))
            type = QStringLiteral("true_false");
    }
    QJsonObject source{{"document", QFileInfo(document.sourcePath).fileName()}};
    if (anchor.line < lines.size() && lines.at(anchor.line).page > 0)
        source.insert("page", lines.at(anchor.line).page);
    QJsonObject question{{"id", stableId},
                         {"catalogId", "generated"},
                         {"type", type},
                         {"stem", stemLines.join(QChar('\n')).trimmed()},
                         {"options", jsonOptions},
                         {"answer", QJsonObject{{"optionIds", answerIds}}},
                         {"solution", solutionLines.join(QChar('\n')).trimmed()},
                         {"source", source}};
    if (!materialId.isEmpty())
        question.insert("materialId", materialId);
    if (!reasons.isEmpty()) {
        *reviewReason = reasons.join(QStringLiteral("；"));
        question.insert(
            "review",
            QJsonObject{{"needsReview", true}, {"confidence", 0.25}, {"reason", *reviewReason}});
    } else {
        question.insert("review", QJsonObject{{"needsReview", false},
                                              {"confidence", document.usedOcr ? 0.75 : 0.95}});
    }
    return question;
}

} // namespace

RuleBasedGenerationResult
RuleBasedBankGenerator::generate(const QList<ExtractedDocument>& documents) const {
    RuleBasedGenerationResult result;
    int documentOrdinal = 0;
    for (const auto& document : documents) {
        ++documentOrdinal;
        const QList<SourceLine> lines = sourceLines(document);

        // 收集所有答案区头行号。整体前后分开的文件只有一个，阶段分组的文件
        // 会有多个（每个阶段一组题+一组答案）。答案区头本身不作为题目终点，
        // 但它界定了“从这里开始进入答案文本”。
        QList<int> answerSections;
        for (int index = 0; index < lines.size(); ++index) {
            if (isAnswerSectionHeader(lines.at(index).text))
                answerSections.append(index);
        }
        const int firstAnswerSection = answerSections.isEmpty() ? -1 : answerSections.first();
        // contentEnd 用于框定材料扫描范围与题目块终点：在没有任何答案区头时，整
        // 篇都是题目；否则题目区止于第一个答案区头。阶段二、三的题目区在其各自
        // 的答案区头之后，会经由 blockEnd 被正确切分。
        const int contentEnd = firstAnswerSection >= 0 ? firstAnswerSection : lines.size();

        // 材料头可能出现在任意阶段，整体扫描而非只扫到第一个答案区头前。
        QList<MaterialMarker> materialMarkers;
        int materialOrdinal = 0;
        static const QRegularExpression rangePattern(
            QStringLiteral(R"((\d{1,4})\s*(?:[-—~～]|至|到)\s*(\d{1,4})\s*题)"));

        // 题号锚点扫描：覆盖阶段分组里出现在答案区头之后的题目。答案区头行不算
        // 锚点；同时排除“1.A”“2.B”这类答案汇总行（题号后紧跟答案字母）。
        // 关键规则：最后一个答案区头之后的候选锚点都属于“末尾答案区”内的解析
        // 文本（如“1. 某选项是第一个”），一律剔除；位于前序答案区头之间的候选
        // 锚点才是阶段二的题目（前一个答案区已结束、下一个答案区未开始）。
        static const QRegularExpression answerRecordLine(QStringLiteral(
            R"(^\s*(?:第\s*)?(\d{1,4})\s*(?:题|[\.．、:：\)）])\s*(?:【?答案】?\s*[:：]?)?\s*[A-Fa-f]{1,6}\s*$)"));
        const int lastAnswerSection = answerSections.isEmpty() ? -1 : answerSections.last();
        QList<QuestionAnchor> anchors;
        for (int index = 0; index < lines.size(); ++index) {
            if (isAnswerSectionHeader(lines.at(index).text))
                continue;
            const QString text = lines.at(index).text;
            if (answerRecordLine.match(text.trimmed()).hasMatch())
                continue;
            // 落在最后一个答案区头之后 → 属于末尾答案区的解析文本，剔除。
            if (lastAnswerSection >= 0 && index > lastAnswerSection)
                continue;
            const auto match = questionPattern().match(text);
            if (!match.hasMatch())
                continue;
            const int number = match.captured(1).toInt();
            if (number <= 0)
                continue;
            anchors.append({index, number, match.captured(2).trimmed()});
        }

        // 材料扫描：材料头与“根据材料回答N-M题”的范围头一起出现，范围头可能与
        // 材料头同段。这里复用既有的范围正则与归属逻辑。
        for (int line = 0; line < lines.size(); ++line) {
            if (!isMaterialHeader(lines.at(line).text))
                continue;
            int firstQuestionLine = -1;
            for (const auto& anchor : anchors) {
                if (anchor.line > line) {
                    firstQuestionLine = anchor.line;
                    break;
                }
            }
            if (firstQuestionLine < 0)
                continue;
            int firstNumber = 0;
            int lastNumber = 0;
            QString rangeText;
            for (int cursor = line; cursor < firstQuestionLine; ++cursor)
                rangeText += lines.at(cursor).text + QChar('\n');
            const auto range = rangePattern.match(rangeText);
            if (range.hasMatch()) {
                firstNumber = range.captured(1).toInt();
                lastNumber = range.captured(2).toInt();
                if (firstNumber > lastNumber)
                    qSwap(firstNumber, lastNumber);
            }
            const QString id =
                QStringLiteral("r%1-m%2").arg(documentOrdinal).arg(++materialOrdinal);
            materialMarkers.append({line, firstQuestionLine, firstNumber, lastNumber, id});
            QStringList body;
            for (int cursor = line + 1; cursor < firstQuestionLine; ++cursor)
                if (!lines.at(cursor).text.trimmed().isEmpty())
                    body.append(lines.at(cursor).text);
            if (body.isEmpty())
                body.append(lines.at(line).text);
            QJsonObject source{{"document", QFileInfo(document.sourcePath).fileName()}};
            if (lines.at(line).page > 0)
                source.insert("page", lines.at(line).page);
            result.materials.append(QJsonObject{{"id", id},
                                                {"catalogId", "generated"},
                                                {"title", lines.at(line).text.left(200)},
                                                {"body", body.join(QChar('\n')).trimmed()},
                                                {"source", source}});
        }

        // 逐段解析答案区。每段的作用域到下一个题号锚点或下一个材料头为止，
        // 这样阶段二的题目不会被阶段一的答案区吞掉。
        QHash<int, QString> answers;
        QHash<int, QString> solutions;
        for (int sectionIndex = 0; sectionIndex < answerSections.size(); ++sectionIndex) {
            const int sectionStart = answerSections.at(sectionIndex);
            const int sectionEnd =
                answerSectionEnd(lines, sectionStart, anchors, materialMarkers);
            const auto segmentAnswers = globalAnswers(lines, sectionStart, sectionEnd);
            for (auto it = segmentAnswers.cbegin(); it != segmentAnswers.cend(); ++it)
                if (!answers.contains(it.key()))
                    answers.insert(it.key(), it.value());
            const auto segmentSolutions = globalSolutions(lines, sectionStart, sectionEnd);
            for (auto it = segmentSolutions.cbegin(); it != segmentSolutions.cend(); ++it)
                if (!solutions.contains(it.key()))
                    solutions.insert(it.key(), it.value());
        }

        if (anchors.isEmpty()) {
            result.warnings.append(QStringLiteral("%1：没有识别到题号锚点")
                                       .arg(QFileInfo(document.sourcePath).fileName()));
            continue;
        }

        int questionOrdinal = 0;
        for (int index = 0; index < anchors.size(); ++index) {
            const QuestionAnchor& anchor = anchors.at(index);
            int blockEnd = index + 1 < anchors.size() ? anchors.at(index + 1).line : lines.size();
            // 题目块不能越过任何答案区头：遇到答案区头说明题目区已结束。
            for (int section : answerSections)
                if (section > anchor.line && section < blockEnd) {
                    blockEnd = section;
                    break;
                }
            blockEnd = nextMaterialLine(materialMarkers, anchor.line, blockEnd);
            const QString id = QStringLiteral("r%1-q%2-%3")
                                   .arg(documentOrdinal)
                                   .arg(anchor.number)
                                   .arg(++questionOrdinal);
            const QString materialId =
                materialIdForQuestion(materialMarkers, anchor.line, anchor.number);
            QString reviewReason;
            QJsonObject question = parseQuestion(document, lines, anchor, blockEnd, id, materialId,
                                                 answers, solutions, &reviewReason);
            if (reviewReason.isEmpty())
                result.questions.append(question);
            else
                result.needsReviewQuestions.append(question);
        }
    }

    // 删除没有任何题目引用的材料，保证正常候选进入统一校验器时不会因孤立材料失败。
    QSet<QString> referenced;
    for (const QJsonArray questions : {result.questions, result.needsReviewQuestions})
        for (const auto& value : questions) {
            const QString id = value.toObject().value("materialId").toString();
            if (!id.isEmpty())
                referenced.insert(id);
        }
    QJsonArray usedMaterials;
    for (const auto& value : result.materials)
        if (referenced.contains(value.toObject().value("id").toString()))
            usedMaterials.append(value);
    result.materials = usedMaterials;
    return result;
}

} // namespace quizpane::studio
