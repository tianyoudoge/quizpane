#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QStringList>

namespace quizpane::studio {

struct SourceFingerprint {
    QString path;
    QString sha256;
};

struct GenerationCheckpoint {
    QString taskId;
    QStringList sourcePaths;
    QList<SourceFingerprint> sources;
    int sourceBlockCount = 0;
    QList<int> completedSourceBlocks;
    QJsonArray materials;
    QJsonArray questions;
    QJsonArray needsReviewQuestions;
    // key 为“源块序号:模型材料 ID”，value 为写入最终题库的稳定 ID。
    QJsonObject materialIdRenames;
    // 最终材料 ID -> 已生成的关联题目 ID，用于恢复后重建材料树。
    QJsonObject materialQuestionIds;
    qint64 inputTokens = 0;
    qint64 outputTokens = 0;
};

class CheckpointStore final {
  public:
    // 根据排序后的绝对源路径生成稳定任务 ID；文件内容变化由 fingerprint 校验。
    QString taskIdForSources(const QStringList& sourcePaths) const;

    // 返回任务检查点的落盘位置，不创建目录或文件。
    QString pathForTask(const QString& taskId) const;

    // 读取所有源文件并创建 SHA-256 快照；任一文件失败时整体失败。
    bool createFingerprints(const QStringList& sourcePaths, QList<SourceFingerprint>* fingerprints,
                            QString* error = nullptr) const;

    // 以 QSaveFile 原子写入 v2 检查点，并收紧为仅当前用户可读写。
    bool save(const GenerationCheckpoint& checkpoint, QString* error = nullptr) const;

    // 加载并严格校验 v2 结构。v1 或字段缺失会明确拒绝，不执行材料关系迁移。
    bool load(const QString& taskId, GenerationCheckpoint* checkpoint,
              QString* error = nullptr) const;

    // 重新计算源文件摘要，判断检查点是否仍可安全复用。
    bool sourcesUnchanged(const GenerationCheckpoint& checkpoint, QString* error = nullptr) const;

    // 幂等删除指定任务检查点；文件不存在也视为成功。
    bool clear(const QString& taskId, QString* error = nullptr) const;
};

} // namespace quizpane::studio
