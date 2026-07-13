#pragma once

#include <QString>
#include <QVector>

namespace quizpane::studio {

// 一个待发给模型的文本块。sourcePath 保留来源，用于修复循环里把错误
// 连同原文片段回传，也用于断点续跑时按块记录进度。index 是块在整份
// 源文件列表中的稳定序号，与 CheckpointStore 里记录的"已完成块"对应。
struct TextChunk {
    int index = 0;
    QString sourcePath;
    QString text;
    qint64 estimatedTokens = 0;
};

// 按 token 预算切分文本。中文字符大约对应更多 token，这里用保守的
// 字符数/2.5 估算（token 统计的唯一权威来源仍是模型返回的 usage 字段，
// 这里只用于决定切块边界，不用于展示给用户的计费信息）。优先在空行
// 或换行处断开，避免把一道题从题干中间切开。
class Chunker final {
public:
    explicit Chunker(qint64 tokenBudget = 2000);
    QVector<TextChunk> split(const QString& sourcePath, const QString& text, int startIndex = 0) const;

private:
    qint64 tokenBudget_;
};

}  // namespace quizpane::studio
