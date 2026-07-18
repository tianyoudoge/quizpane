#include "quizpane/studio/generation_workflow.hpp"

#include "quizpane/diagnostic_logger.hpp"
#include "quizpane/studio/document_extractor.hpp"
#include "quizpane/studio/rule_based_generator.hpp"

#include <QFileInfo>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QPointer>
#include <QtConcurrent/QtConcurrentRun>

namespace quizpane::studio {

GenerationWorkflow::GenerationWorkflow(QObject* parent) : QObject(parent) {}

void GenerationWorkflow::startRuleBased(const QStringList& sourcePaths) {
    QList<SourceMaterialGroup> groups;
    for (const QString& path : sourcePaths)
        groups.append({path, {}, true});
    startRuleBased(groups);
}

void GenerationWorkflow::startRuleBased(const QList<SourceMaterialGroup>& sources) {
    if (active_)
        return;
    diagnostic::event(QStringLiteral("workflow"), QStringLiteral("start"),
         {{QStringLiteral("mode"), QStringLiteral("rules")},
         {QStringLiteral("sources"), sources.size()}});
    active_ = true;

    publish(WorkflowStage::Extracting,
            QStringLiteral("正在本地读取 %1 份资料…").arg(sources.size()));
    // PDF 渲染、OCR 和规则扫描都会触发大量 CPU/磁盘工作。放到工作线程后，主窗口
    // 的“运行中”动画能持续刷新，完成结果再排回 GUI 线程，避免跨线程操作控件。
    const QPointer<GenerationWorkflow> owner(this);
    const bool hasAnswerKey = sources.isEmpty() || sources.first().hasAnswerKey;
    [[maybe_unused]] const auto backgroundTask = QtConcurrent::run([owner, sources, hasAnswerKey] {
        const auto publishProgress = [owner](WorkflowStage stage, int completed, int total,
                                             const QString& detail) {
            if (!owner)
                return;
            QMetaObject::invokeMethod(owner.data(), [owner, stage, completed, total, detail] {
                if (owner && owner->active_)
                    emit owner->progressChanged({stage, completed, total, 0, 0, detail});
            }, Qt::QueuedConnection);
        };
        QElapsedTimer elapsed;
        elapsed.start();
        QList<ExtractedDocument> documents;
        ExtractorRegistry registry;
        QString failure;
        for (qsizetype sourceIndex = 0; sourceIndex < sources.size(); ++sourceIndex) {
            const SourceMaterialGroup& source = sources.at(sourceIndex);
            if (source.hasAnswerKey != hasAnswerKey) {
                failure = QStringLiteral("同一题库不能混合含答案与无答案资料");
                break;
            }
            const QString path = source.questionPath;
            publishProgress(WorkflowStage::Extracting, sourceIndex, sources.size(),
                            QStringLiteral("正在读取第 %1 / %2 份资料：%3")
                                .arg(sourceIndex + 1).arg(sources.size())
                                .arg(QFileInfo(path).fileName()));
            ExtractedDocument document = registry.extract(QFileInfo(path).absoluteFilePath());
            if (!document.error.isEmpty()) {
                failure = QStringLiteral("%1：%2").arg(QFileInfo(path).fileName(), document.error);
                break;
            }
            if (!source.answerPath.isEmpty()) {
                if (!hasAnswerKey) {
                    failure = QStringLiteral("无答案题库不能配对答案文件");
                    break;
                }
                const ExtractedDocument answers = registry.extract(
                    QFileInfo(source.answerPath).absoluteFilePath());
                if (!answers.error.isEmpty()) {
                    failure = QStringLiteral("%1：%2")
                        .arg(QFileInfo(source.answerPath).fileName(), answers.error);
                    break;
                }
                document.plainText += QStringLiteral("\n\n答案及解析\n") + answers.plainText;
            }
            documents.append(document);
            publishProgress(WorkflowStage::Extracting, sourceIndex + 1, sources.size(),
                            QStringLiteral("已读取第 %1 / %2 份资料，正在继续处理…")
                                .arg(sourceIndex + 1).arg(sources.size()));
        }
        publishProgress(WorkflowStage::Chunking, documents.size(), qMax(1, documents.size()),
                        QStringLiteral("资料读取完成，正在按题号、选项、答案和材料规则整理…"));
        QElapsedTimer generationElapsed;
        generationElapsed.start();
        const RuleBasedGenerationResult result = failure.isEmpty()
            ? RuleBasedBankGenerator{}.generate(documents, hasAnswerKey) : RuleBasedGenerationResult{};
        diagnostic::event(QStringLiteral("workflow"), QStringLiteral("rule-run-finished"),
            {{QStringLiteral("sources"), documents.size()},
             {QStringLiteral("generationMs"), generationElapsed.elapsed()},
             {QStringLiteral("totalMs"), elapsed.elapsed()}});
        if (!owner)
            return;
        QMetaObject::invokeMethod(owner.data(), [owner, result, failure] {
            if (!owner || !owner->active_)
                return; // 用户已经取消或关闭了任务。
            GenerationWorkflow* self = owner.data();
            if (!failure.isEmpty()) {
                self->active_ = false;
                self->publish(WorkflowStage::Failed, failure);
                emit self->failed(failure);
                return;
            }
            self->publish(WorkflowStage::Chunking, QStringLiteral("正在按题号、选项、答案和材料规则解析"));
            if (result.questions.isEmpty() && result.needsReviewQuestions.isEmpty()) {
                self->active_ = false;
                const QString detail = result.warnings.isEmpty()
                    ? QStringLiteral("规则引擎没有识别到题目") : result.warnings.join(QStringLiteral("；"));
                self->publish(WorkflowStage::Failed, detail);
                emit self->failed(detail);
                return;
            }
            self->active_ = false;
            const QString detail = QStringLiteral("规则解析完成：%1 道可直接使用，%2 道待复核")
                .arg(result.questions.size()).arg(result.needsReviewQuestions.size());
            self->publish(WorkflowStage::Done, detail);
            emit self->questionsReady({result.materials, result.questions,
                                       result.needsReviewQuestions, result.warnings,
                                       result.assets, result.hasAnswerKey});
            emit self->finished();
        }, Qt::QueuedConnection);
    });
}

void GenerationWorkflow::cancel() {
    active_ = false;
}

void GenerationWorkflow::publish(WorkflowStage stage, const QString& detail) {
    diagnostic::event(QStringLiteral("workflow"), QStringLiteral("progress"),
        {{QStringLiteral("stage"), static_cast<int>(stage)},
         {QStringLiteral("detail"), detail}});
    emit progressChanged({stage, 0, 0, 0, 0, detail});
}

} // namespace quizpane::studio
