#include "quizpane/studio/chunker.hpp"

#include <QRegularExpression>

namespace quizpane::studio {
namespace {

qint64 estimateTokens(const QString& text) {
    // 保守估算：中英文混排场景下，字符数/2.5 通常略高于真实 token 数，
    // 避免因为低估导致单块超出模型上下文窗口。
    return qMax<qint64>(1, qint64(text.size() / 2.5));
}

}  // namespace

Chunker::Chunker(qint64 tokenBudget) : tokenBudget_(tokenBudget) {}

QVector<TextChunk> Chunker::split(const QString& sourcePath, const QString& text, int startIndex) const {
    QVector<TextChunk> chunks;
    if (text.isEmpty()) return chunks;

    // 按空行拆出段落，再把段落贪心地装进不超过预算的块里。单个段落本身
    // 超过预算时单独成块，不再继续细分——过度切碎会让模型缺少足够上下文
    // 判断题目边界。
    const QStringList paragraphs = text.split(QRegularExpression("\n\\s*\n"), Qt::SkipEmptyParts);
    QString current;
    qint64 currentTokens = 0;
    int index = startIndex;

    const auto flush = [&] {
        if (current.trimmed().isEmpty()) return;
        chunks.append({index++, sourcePath, current.trimmed(), currentTokens});
        current.clear();
        currentTokens = 0;
    };

    for (const QString& paragraph : paragraphs) {
        // 单段超预算时按字符上限继续切，保证异常长的 Markdown 段落不会
        // 直接突破模型上下文。2.5 与 estimateTokens 使用同一保守换算。
        const qsizetype maxCharacters = qMax<qsizetype>(1, qsizetype(tokenBudget_ * 2.5));
        qsizetype offset = 0;
        while (offset < paragraph.size()) {
            const QString part = paragraph.mid(offset, maxCharacters);
            const qint64 partTokens = estimateTokens(part);
            if (!current.isEmpty() && currentTokens + partTokens > tokenBudget_) flush();
            if (!current.isEmpty()) current += QStringLiteral("\n\n");
            current += part;
            currentTokens += partTokens;
            offset += part.size();
            if (offset < paragraph.size()) flush();
        }
    }
    flush();
    return chunks;
}

}  // namespace quizpane::studio
