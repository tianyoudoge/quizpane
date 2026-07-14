#include "quizpane/studio/rule_based_generator.hpp"

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

QString normalizeAnswer(QString answer) {
    answer = answer.normalized(QString::NormalizationForm_KC).trimmed().toUpper();
    answer.remove(QRegularExpression(QStringLiteral("[\\s,，、;；/|+]+")));
    if (!QRegularExpression(QStringLiteral("^[A-F]{1,6}$")).match(answer).hasMatch())
        return {};
    QString normalized;
    for (const QChar ch : answer)
        if (!normalized.contains(ch))
            normalized += ch;
    return normalized;
}

QString comparableText(QString value) {
    value = value.normalized(QString::NormalizationForm_KC).toLower().trimmed();
    value.remove(QRegularExpression(QStringLiteral("[\\s\\p{P}\\p{S}]+")));
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
    const QString normalized = document.plainText.normalized(QString::NormalizationForm_KC);
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

QHash<int, QString> globalAnswers(const QList<SourceLine>& lines, int sectionStart) {
    QHash<int, QString> answers;
    if (sectionStart < 0)
        return answers;
    static const QRegularExpression pair(QStringLiteral(
        R"((?:^|[\s,，;；])(?:第\s*)?(\d{1,4})\s*(?:题|[\.．、:：\)）-])?\s*(?:【?答案】?\s*[:：]?)?\s*([A-Fa-f]{1,6})(?=$|[\s,，;；]))"));
    static const QRegularExpression range(
        QStringLiteral(R"((\d{1,4})\s*(?:[-—~～]|至|到)\s*(\d{1,4})\s*[:：]?\s*([A-Fa-f]+))"));
    for (int index = sectionStart + 1; index < lines.size(); ++index) {
        const QString line = lines.at(index).text;
        const auto rangeMatch = range.match(line);
        if (rangeMatch.hasMatch()) {
            const int first = rangeMatch.captured(1).toInt();
            const int last = rangeMatch.captured(2).toInt();
            const QString values = rangeMatch.captured(3).toUpper();
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
    }
    return answers;
}

QHash<int, QString> globalSolutions(const QList<SourceLine>& lines, int sectionStart) {
    QHash<int, QString> solutions;
    if (sectionStart < 0)
        return solutions;
    static const QRegularExpression record(QStringLiteral(
        R"(^\s*(?:第\s*)?(\d{1,4})\s*(?:题|[\.．、\)）])\s*(?:【?答案】?\s*[:：]?)?\s*[A-Fa-f]{1,6}\s*(.*)$)"));
    int currentNumber = 0;
    bool collecting = false;
    for (int index = sectionStart + 1; index < lines.size(); ++index) {
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

QList<QuestionAnchor> questionAnchors(const QList<SourceLine>& lines, int contentEnd) {
    QList<QuestionAnchor> anchors;
    for (int index = 0; index < contentEnd; ++index) {
        const auto match = questionPattern().match(lines.at(index).text);
        if (!match.hasMatch())
            continue;
        const int number = match.captured(1).toInt();
        if (number <= 0)
            continue;
        anchors.append({index, number, match.captured(2).trimmed()});
    }
    return anchors;
}

QList<QPair<QString, QString>> optionsOnLine(const QString& line) {
    static const QRegularExpression marker(
        QStringLiteral(R"((?<![A-Za-z0-9])([A-Fa-f])\s*[\.．、:：\)）]\s*)"));
    QList<QRegularExpressionMatch> markers;
    auto iterator = marker.globalMatch(line);
    while (iterator.hasNext())
        markers.append(iterator.next());
    if (markers.isEmpty() || !line.left(markers.first().capturedStart()).trimmed().isEmpty())
        return {};
    QList<QPair<QString, QString>> result;
    for (int index = 0; index < markers.size(); ++index) {
        const int start = markers.at(index).capturedEnd();
        const int end =
            index + 1 < markers.size() ? markers.at(index + 1).capturedStart() : line.size();
        const QString text = line.mid(start, end - start).trimmed();
        if (!text.isEmpty())
            result.append({markers.at(index).captured(1).toLower(), text});
    }
    return result;
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
    if (!anchor.firstStemLine.isEmpty())
        stemLines.append(anchor.firstStemLine);
    QList<QPair<QString, QString>> options;
    QString rawAnswer;
    QStringList solutionLines;
    bool inSolution = false;
    bool afterOptions = false;

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
        const auto parsedOptions = optionsOnLine(line);
        if (!parsedOptions.isEmpty()) {
            options += parsedOptions;
            afterOptions = true;
        } else if (afterOptions && !options.isEmpty()) {
            options.last().second += QStringLiteral("\n") + line;
        } else {
            stemLines.append(line);
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
        int answerSection = -1;
        for (int index = 0; index < lines.size(); ++index) {
            if (isAnswerSectionHeader(lines.at(index).text)) {
                answerSection = index;
                break;
            }
        }
        const int contentEnd = answerSection >= 0 ? answerSection : lines.size();
        const auto anchors = questionAnchors(lines, contentEnd);
        const auto answers = globalAnswers(lines, answerSection);
        const auto solutions = globalSolutions(lines, answerSection);
        if (anchors.isEmpty()) {
            result.warnings.append(QStringLiteral("%1：没有识别到题号锚点")
                                       .arg(QFileInfo(document.sourcePath).fileName()));
            continue;
        }

        QList<MaterialMarker> materialMarkers;
        int materialOrdinal = 0;
        static const QRegularExpression rangePattern(
            QStringLiteral(R"((\d{1,4})\s*(?:[-—~～]|至|到)\s*(\d{1,4})\s*题)"));
        for (int line = 0; line < contentEnd; ++line) {
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

        int questionOrdinal = 0;
        for (int index = 0; index < anchors.size(); ++index) {
            const QuestionAnchor& anchor = anchors.at(index);
            int blockEnd = index + 1 < anchors.size() ? anchors.at(index + 1).line : contentEnd;
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
                result.normalQuestions.append(question);
            else
                result.needsReviewQuestions.append(question);
        }
    }

    // 删除没有任何题目引用的材料，保证正常候选进入统一校验器时不会因孤立材料失败。
    QSet<QString> referenced;
    for (const QJsonArray questions : {result.normalQuestions, result.needsReviewQuestions})
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
