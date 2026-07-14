#include "quizpane/studio/chunker.hpp"

#include <QRegularExpression>

namespace quizpane::studio {
namespace {

qint64 estimateTokens(const QString& text) {
    // 保守估算：中英文混排场景下，字符数/2.5 通常略高于真实 token 数，
    // 避免因为低估导致单块超出模型上下文窗口。
    return qMax<qint64>(1, qint64(text.size() / 2.5));
}

bool isMaterialHeading(const QString& paragraph) {
    static const QRegularExpression materialHeading(
        QStringLiteral(R"(^\s*(?:#{1,6}\s*)?材料\s*(?:[一二三四五六七八九十]+|\d+)\b)"));
    return materialHeading.match(paragraph).hasMatch();
}

bool isDocumentHeading(const QString& paragraph) {
    static const QRegularExpression pattern(QStringLiteral(R"((?:^|\n)\s*(?:文档|篇章)\s*\d+\b)"));
    return pattern.match(paragraph).hasMatch();
}

bool isOriginalTextHeading(const QString& paragraph) {
    static const QRegularExpression pattern(QStringLiteral(R"(^\s*原文\s*[:：])"));
    return pattern.match(paragraph).hasMatch();
}

bool isGroupedQuestionCue(const QString& paragraph) {
    static const QRegularExpression groupedQuestionCue(QStringLiteral(
        R"((?:根据|阅读).{0,24}(?:资料|材料|短文|文章).{0,24}(?:回答|完成).{0,16}(?:第\s*)?\d+\s*(?:[～~—-]|至)\s*\d+\s*题)"));
    return groupedQuestionCue.match(paragraph).hasMatch() || isDocumentHeading(paragraph) ||
           isOriginalTextHeading(paragraph);
}

bool isMaterialStart(const QString& paragraph) {
    return isMaterialHeading(paragraph) || isGroupedQuestionCue(paragraph);
}

bool isBoundaryHeading(const QString& paragraph) {
    static const QRegularExpression markdownHeading(QStringLiteral(R"(^\s*#{1,6}\s+\S)"));
    return markdownHeading.match(paragraph).hasMatch();
}

bool isQuestionWithOptions(const QString& paragraph) {
    static const QRegularExpression numbered(QStringLiteral(R"(^\s*\d+[.、．]\s*\S)"));
    static const QRegularExpression options(QStringLiteral(R"((?:^|\n)\s*[A-DＡ-Ｄ][.、．]\s*\S)"));
    return numbered.match(paragraph).hasMatch() && options.match(paragraph).hasMatch();
}

QStringList paragraphsOf(const QString& text) {
    return text.split(QRegularExpression(QStringLiteral("\n\\s*\n")), Qt::SkipEmptyParts);
}

} // namespace

Chunker::Chunker(qint64 tokenBudget) : tokenBudget_(tokenBudget) {}

QVector<SourceBlock> Chunker::blocks(const QString& sourcePath, const QString& text,
                                     int startIndex) const {
    QVector<SourceBlock> result;
    const QStringList paragraphs = paragraphsOf(text);
    const qsizetype maxCharacters = qMax<qsizetype>(1, qsizetype(tokenBudget_ * 2.5));
    int index = startIndex;
    for (qsizetype position = 0; position < paragraphs.size();) {
        if (isMaterialStart(paragraphs.at(position))) {
            const bool startsWithCue = isGroupedQuestionCue(paragraphs.at(position));
            const bool startsWithDocumentHeading = isDocumentHeading(paragraphs.at(position));
            QStringList group{paragraphs.at(position++)};
            while (
                position < paragraphs.size() &&
                !(isMaterialHeading(paragraphs.at(position)) && !startsWithCue) &&
                !(isGroupedQuestionCue(paragraphs.at(position)) &&
                  !(startsWithDocumentHeading && isOriginalTextHeading(paragraphs.at(position)))) &&
                !isBoundaryHeading(paragraphs.at(position))) {
                group.append(paragraphs.at(position++));
            }
            const QString textValue = group.join(QStringLiteral("\n\n")).trimmed();
            result.append({index++, sourcePath, textValue, estimateTokens(textValue), true});
            continue;
        }

        if (isBoundaryHeading(paragraphs.at(position))) {
            QStringList section{paragraphs.at(position++)};
            while (position < paragraphs.size() && !isBoundaryHeading(paragraphs.at(position)) &&
                   !isMaterialStart(paragraphs.at(position)))
                section.append(paragraphs.at(position++));
            const QString textValue = section.join(QStringLiteral("\n\n")).trimmed();
            result.append({index++, sourcePath, textValue, estimateTokens(textValue), true});
            continue;
        }

        const QString paragraph = paragraphs.at(position++).trimmed();
        if (isQuestionWithOptions(paragraph)) {
            result.append({index++, sourcePath, paragraph, estimateTokens(paragraph), true});
            continue;
        }
        for (qsizetype offset = 0; offset < paragraph.size(); offset += maxCharacters) {
            const QString part = paragraph.mid(offset, maxCharacters);
            result.append({index++, sourcePath, part, estimateTokens(part), false});
        }
    }
    return result;
}

QVector<TextChunk> Chunker::split(const QString& sourcePath, const QString& text,
                                  int startIndex) const {
    QVector<TextChunk> chunks;
    const auto sourceBlocks = blocks(sourcePath, text, startIndex);
    TextChunk current;
    current.sourcePath = sourcePath;
    int requestIndex = 0;
    const auto flush = [&] {
        if (current.sourceBlockIndices.isEmpty())
            return;
        current.index = requestIndex++;
        current.text = current.text.trimmed();
        chunks.append(current);
        current = {};
        current.sourcePath = sourcePath;
    };
    for (const SourceBlock& block : sourceBlocks) {
        if (!current.sourceBlockIndices.isEmpty() &&
            (block.indivisible || current.estimatedTokens + block.estimatedTokens > tokenBudget_)) {
            flush();
        }
        if (!current.text.isEmpty())
            current.text += QStringLiteral("\n\n");
        current.text += block.text;
        current.estimatedTokens += block.estimatedTokens;
        current.sourceBlockIndices.append(block.index);
        if (block.indivisible || current.estimatedTokens >= tokenBudget_)
            flush();
    }
    flush();
    return chunks;
}

} // namespace quizpane::studio
