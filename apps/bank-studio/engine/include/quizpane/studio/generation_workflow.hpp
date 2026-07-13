#pragma once

#include "model_settings_dialog.hpp"
#include "quizpane/studio/checkpoint_store.hpp"
#include "quizpane/studio/chunker.hpp"
#include "quizpane/studio/model_client.hpp"

#include <QJsonArray>
#include <QObject>
#include <QSet>

class QNetworkAccessManager;

namespace quizpane::studio {

enum class WorkflowStage {
    Idle, Extracting, Chunking, Generating, Validating, Repairing,
    Packaging, Done, Failed
};

struct WorkflowProgress {
    WorkflowStage stage = WorkflowStage::Idle;
    int completedChunks = 0;
    int totalChunks = 0;
    qint64 inputTokens = 0;
    qint64 outputTokens = 0;
    QString detail;
};

class GenerationWorkflow final : public QObject {
    Q_OBJECT
public:
    explicit GenerationWorkflow(QNetworkAccessManager* manager, QObject* parent = nullptr);
    void start(const QStringList& sourcePaths, const ModelSettings& settings,
               const QString& resumeTaskId = {});
    void pause();
    void cancel();
    bool isActive() const { return active_; }
    QString taskId() const { return checkpoint_.taskId; }
    const GenerationCheckpoint& checkpoint() const { return checkpoint_; }

signals:
    void progressChanged(const quizpane::studio::WorkflowProgress& progress);
    void questionsReady(const QJsonArray& questions, const QJsonArray& needsReviewQuestions);
    void failed(const QString& error);
    void finished();

private:
    void processNextChunk();
    void requestChunk(bool repair, const QStringList& validationErrors = {});
    void handleModelResult(const GenerationResult& result);
    QJsonArray parseQuestions(const QString& rawText, QString* error) const;
    QJsonObject bankForQuestions(const QJsonArray& questions) const;
    void completeChunk(const QJsonArray& valid, const QJsonArray& review);
    void publish(WorkflowStage stage, const QString& detail);
    void failWorkflow(const QString& error);

    ModelClient client_;
    CheckpointStore store_;
    QVector<TextChunk> chunks_;
    ModelSettings settings_;
    GenerationCheckpoint checkpoint_;
    int currentChunkPosition_ = -1;
    bool active_ = false;
    bool paused_ = false;
    bool repairing_ = false;
};

}  // namespace quizpane::studio

Q_DECLARE_METATYPE(quizpane::studio::WorkflowProgress)
Q_DECLARE_METATYPE(quizpane::studio::GenerationResult)
