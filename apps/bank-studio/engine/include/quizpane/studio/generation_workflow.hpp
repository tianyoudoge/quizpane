#pragma once

#include "model_settings_dialog.hpp"
#include "quizpane/studio/checkpoint_store.hpp"
#include "quizpane/studio/chunker.hpp"
#include "quizpane/studio/model_client.hpp"
#include "quizpane/studio/review_result.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QSet>

class QNetworkAccessManager;

namespace quizpane::studio {

enum class WorkflowStage {
    Idle,
    Extracting,
    Chunking,
    Generating,
    Validating,
    Repairing,
    Packaging,
    Done,
    Failed
};

struct WorkflowProgress {
    WorkflowStage stage = WorkflowStage::Idle;
    int completedSourceBlocks = 0;
    int totalSourceBlocks = 0;
    qint64 inputTokens = 0;
    qint64 outputTokens = 0;
    QString detail;
};

using GeneratedBankCandidate = ReviewResult;

// 一组资料可由题目文件和独立答案/解析文件组成。离线路径在本机合并两者，
// 按题号匹配答案；没有答案文件时 answerPath 为空。
struct SourceMaterialGroup {
    QString questionPath;
    QString answerPath;
};

// 模型输出解析是独立的确定性边界：网络状态机和 JSON/Schema 规则互不耦合。
GeneratedBankCandidate parseGeneratedBankCandidate(const QString& rawText,
                                                   QString* error = nullptr);

class GenerationWorkflow final : public QObject {
    Q_OBJECT
  public:
    // manager 是外部网络基础设施，生命周期必须覆盖本工作流；工作流自身拥有
    // ModelClient 和 CheckpointStore，类似 application service 组合 gateway/repository。
    explicit GenerationWorkflow(QNetworkAccessManager* manager, QObject* parent = nullptr);

    // 启动或恢复生成任务：校验检查点、提取文档、语义切块，然后逐块串行调用模型。
    // 活跃任务上的重复调用会被忽略，所有失败通过 failed 信号返回。
    void start(const QStringList& sourcePaths, const ModelSettings& settings,
               const QString& resumeTaskId = {});

    // 完全离线的规则结构化入口：复用同一批提取器和最终候选 DTO，但不创建模型
    // 请求或模型检查点。相同输入始终产生相同输出，适合规整题库快速导入。
    void startRuleBased(const QStringList& sourcePaths);
    void startRuleBased(const QList<SourceMaterialGroup>& sources);

    // 取消当前网络请求并原子保存进度，任务可由相同源文件再次恢复。
    void pause();

    // 取消当前网络请求并删除检查点，之后不能恢复本次任务。
    void cancel();

    // 返回当前任务是否仍可能发起网络请求。
    bool isActive() const {
        return active_;
    }

    // 返回当前检查点任务 ID；尚未建立任务时为空。
    QString taskId() const {
        return checkpoint_.taskId;
    }

    // 返回只读检查点快照，主要供任务状态展示和诊断使用。
    const GenerationCheckpoint& checkpoint() const {
        return checkpoint_;
    }

  signals:
    void progressChanged(const quizpane::studio::WorkflowProgress& progress);
    void questionsReady(const quizpane::studio::GeneratedBankCandidate& candidate);
    void failed(const QString& error);
    void finished();

  private:
    // 跳过已完成 SourceBlock，选择下一个请求；全部完成时发布候选 DTO。
    void processNextChunk();

    // 构造首次生成或修复请求。修复请求包含原文、上一轮完整输出和结构化错误。
    void requestChunk(bool repair, const QStringList& validationErrors = {});

    // 消费一次模型结果并驱动 parse -> validate -> repair/complete 状态迁移。
    void handleModelResult(const GenerationResult& result);

    // 将候选 DTO 包装成唯一 schemaVersion=2 的完整题库对象供规则校验器消费。
    QJsonObject bankForCandidate(const GeneratedBankCandidate& candidate) const;

    // 为材料和题目生成跨块稳定 ID，修正引用，更新关联索引并原子提交检查点。
    void completeChunk(GeneratedBankCandidate candidate);

    // 发布不持久化的进度快照，token 数只采用模型响应中的 usage。
    void publish(WorkflowStage stage, const QString& detail);

    // 终止网络活动、尽力保存现有进度并广播统一失败事件。
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
    QString lastRawOutput_;
};

} // namespace quizpane::studio

Q_DECLARE_METATYPE(quizpane::studio::WorkflowProgress)
Q_DECLARE_METATYPE(quizpane::studio::GenerationResult)
Q_DECLARE_METATYPE(quizpane::studio::GeneratedBankCandidate)
