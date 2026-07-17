#include "quizpane/studio/generation_workflow.hpp"

#include "quizpane/diagnostic_logger.hpp"
#include "quizpane/studio/document_extractor.hpp"
#include "quizpane/studio/rule_based_generator.hpp"

#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QtConcurrent/QtConcurrentRun>

namespace quizpane::studio {

GenerationWorkflow::GenerationWorkflow(QObject* parent) : QObject(parent) {}

void GenerationWorkflow::startRuleBased(const QStringList& sourcePaths) {
    QList<SourceMaterialGroup> groups;
    for (const QString& path : sourcePaths)
        groups.append({path, {}});
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
    [[maybe_unused]] const auto backgroundTask = QtConcurrent::run([owner, sources] {
        QList<ExtractedDocument> documents;
        ExtractorRegistry registry;
        QString failure;
        for (const SourceMaterialGroup& source : sources) {
            const QString path = source.questionPath;
            ExtractedDocument document = registry.extract(QFileInfo(path).absoluteFilePath());
            if (!document.error.isEmpty()) {
                failure = QStringLiteral("%1：%2").arg(QFileInfo(path).fileName(), document.error);
                break;
            }
            if (!source.answerPath.isEmpty()) {
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
        }
        const RuleBasedGenerationResult result = failure.isEmpty()
            ? RuleBasedBankGenerator{}.generate(documents) : RuleBasedGenerationResult{};
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
                                       result.assets});
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
