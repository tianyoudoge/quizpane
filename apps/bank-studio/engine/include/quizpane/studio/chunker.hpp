#pragma once

#include <QString>
#include <QVector>

namespace quizpane::studio {

// 文档语义切分后的最小恢复单元。材料提示语、材料正文及其连续子题会被识别成
// 一个 indivisible 块；普通段落则仍可按 token 预算进一步切分。
struct SourceBlock {
    int index = 0;
    QString sourcePath;
    QString text;
    qint64 estimatedTokens = 0;
    bool indivisible = false;
};

// 一个待发给模型的文本块。sourcePath 保留来源，用于修复循环里把错误
// 连同原文片段回传。sourceBlockIndices 是本次请求包含的稳定恢复单元，
// CheckpointStore 以它们而不是字符切片序号记录完成进度。
struct TextChunk {
    int index = 0;
    QString sourcePath;
    QString text;
    qint64 estimatedTokens = 0;
    QVector<int> sourceBlockIndices;
};

// 按 token 预算切分文本。中文字符大约对应更多 token，这里用保守的
// 字符数/2.5 估算（token 统计的唯一权威来源仍是模型返回的 usage 字段，
// 这里只用于决定切块边界，不用于展示给用户的计费信息）。优先在空行
// 或换行处断开，避免把一道题从题干中间切开。
class Chunker final {
  public:
    // 创建无状态切块器。tokenBudget 只用于请求边界估算，不参与计费统计。
    explicit Chunker(qint64 tokenBudget = 2000);

    // 将单份文档识别成稳定 SourceBlock。startIndex 由上层跨文件累加，确保
    // 检查点中的块 ID 在整个任务内唯一；返回值不会包含空块。
    QVector<SourceBlock> blocks(const QString& sourcePath, const QString& text,
                                int startIndex = 0) const;

    // 将 SourceBlock 贪心装配为模型请求。indivisible 块即使超过预算也保持
    // 完整，调用方可通过 sourceBlockIndices 原子地提交完成进度。
    QVector<TextChunk> split(const QString& sourcePath, const QString& text,
                             int startIndex = 0) const;

  private:
    qint64 tokenBudget_;
};

} // namespace quizpane::studio
