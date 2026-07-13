#pragma once

#include <QJsonArray>
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
    QList<int> completedChunks;
    QJsonArray questions;
    QJsonArray needsReviewQuestions;
    qint64 inputTokens = 0;
    qint64 outputTokens = 0;
};

class CheckpointStore final {
public:
    QString taskIdForSources(const QStringList& sourcePaths) const;
    QString pathForTask(const QString& taskId) const;
    bool createFingerprints(const QStringList& sourcePaths,
                            QList<SourceFingerprint>* fingerprints,
                            QString* error = nullptr) const;
    bool save(const GenerationCheckpoint& checkpoint, QString* error = nullptr) const;
    bool load(const QString& taskId, GenerationCheckpoint* checkpoint,
              QString* error = nullptr) const;
    bool sourcesUnchanged(const GenerationCheckpoint& checkpoint,
                          QString* error = nullptr) const;
    bool clear(const QString& taskId, QString* error = nullptr) const;
};

}  // namespace quizpane::studio
