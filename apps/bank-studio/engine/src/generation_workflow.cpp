#include "quizpane/studio/generation_workflow.hpp"

#include "quizpane/diagnostic_logger.hpp"
#include "quizpane/studio/document_extractor.hpp"
#include "quizpane/studio/mineru_output_adapter.hpp"
#include "quizpane/studio/rule_based_generator.hpp"

#include <QFileInfo>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QPointer>
#include <QtConcurrent/QtConcurrentRun>

#include <optional>

namespace quizpane::studio {
namespace {

constexpr int kAutoAnswerCoveragePercent = 5;

QPair<int, int> resolvedAnswerCoverage(const RuleBasedGenerationResult& result) {
    int resolved = 0;
    int total = 0;
    const auto count = [&resolved, &total](const QJsonArray& questions) {
        for (const QJsonValue& value : questions) {
            ++total;
            if (!value.toObject().value(QStringLiteral("answer")).toObject()
                     .value(QStringLiteral("optionIds")).toArray().isEmpty())
                ++resolved;
        }
    };
    count(result.questions);
    count(result.needsReviewQuestions);
    return {resolved, total};
}

}  // namespace

GenerationWorkflow::GenerationWorkflow(QObject* parent) : QObject(parent) {}

void GenerationWorkflow::startRuleBased(const QStringList& sourcePaths) {
    QList<SourceMaterialGroup> groups;
    for (const QString& path : sourcePaths)
        groups.append({path, {}, AnswerPolicyHint::Auto});
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
        const auto publishSnapshot = [owner](const WorkflowProgress& snapshot) {
            if (!owner)
                return;
            QMetaObject::invokeMethod(owner.data(), [owner, snapshot] {
                if (owner && owner->active_)
                    emit owner->progressChanged(snapshot);
            }, Qt::QueuedConnection);
        };
        const auto publishProgress = [owner](WorkflowStage stage, int completed, int total,
                                             const QString& detail) {
            if (!owner)
                return;
            QMetaObject::invokeMethod(owner.data(), [owner, stage, completed, total, detail] {
                if (owner && owner->active_)
                    emit owner->progressChanged({stage, completed, total, detail});
            }, Qt::QueuedConnection);
        };
        QElapsedTimer elapsed;
        elapsed.start();
        QList<ExtractedDocument> documents;
        ExtractorRegistry registry;
        QString failure;
        std::optional<bool> forcedHasAnswerKey;
        for (const SourceMaterialGroup& source : sources) {
            AnswerPolicyHint policy = source.answerPolicy;
            if (!source.answerPath.isEmpty()) {
                if (policy == AnswerPolicyHint::None) {
                    failure = QStringLiteral("无答案题库不能配对答案文件");
                    break;
                }
                policy = AnswerPolicyHint::Included;
            }
            if (policy == AnswerPolicyHint::Auto)
                continue;
            const bool included = policy == AnswerPolicyHint::Included;
            if (forcedHasAnswerKey.has_value() && *forcedHasAnswerKey != included) {
                failure = QStringLiteral("同一题库不能混合含答案与无答案资料");
                break;
            }
            forcedHasAnswerKey = included;
        }
        for (qsizetype sourceIndex = 0; sourceIndex < sources.size(); ++sourceIndex) {
            if (!failure.isEmpty())
                break;
            const SourceMaterialGroup& source = sources.at(sourceIndex);
            const QString path = source.questionPath;
            publishProgress(WorkflowStage::Extracting, sourceIndex, sources.size(),
                            QStringLiteral("正在读取第 %1 / %2 份资料：%3")
                                .arg(sourceIndex + 1).arg(sources.size())
                                .arg(QFileInfo(path).fileName()));
            // 云解析结果存在时优先使用：它与本地提取产出同一个 ExtractedDocument，
            // 因而下游规则引擎无需知道文档感知来自云端还是本机。
            const auto extractOne = [&registry](const QString& filePath,
                                                const QString& zipPath) {
                const QString absolute = QFileInfo(filePath).absoluteFilePath();
                if (zipPath.isEmpty())
                    return registry.extract(absolute);
                MineruAdaptResult adapted = adaptMineruZip(zipPath, absolute);
                if (!adapted.error.isEmpty()) {
                    ExtractedDocument failed;
                    failed.sourcePath = absolute;
                    failed.error = adapted.error;
                    return failed;
                }
                adapted.document.warnings.append(
                    QStringLiteral("本份资料由 MinerU 云解析（%1 %2）识别，请重点核对图表与表格")
                        .arg(adapted.backend, adapted.versionName));
                return adapted.document;
            };
            ExtractedDocument document = extractOne(path, source.mineruZipPath);
            if (!document.error.isEmpty()) {
                failure = QStringLiteral("%1：%2").arg(QFileInfo(path).fileName(), document.error);
                break;
            }
            if (!source.answerPath.isEmpty()) {
                const ExtractedDocument answers =
                    extractOne(source.answerPath, source.mineruAnswerZipPath);
                if (!answers.error.isEmpty()) {
                    failure = QStringLiteral("%1：%2")
                        .arg(QFileInfo(source.answerPath).fileName(), answers.error);
                    break;
                }
                document.companionAnswerText = answers.plainText;
                document.companionAnswerSourcePath = answers.sourcePath;
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
        RuleBasedGenerationResult result;
        if (failure.isEmpty()) {
            // Auto 可能先按含答案探测、再按无答案语义重跑。把会话挂在工作流持有
            // 的原始文档上，两遍生成和全部分套共用同一个 QPdfDocument，不能在
            // 第二遍又重新打开一次 Qt5/PDFium。
            for (ExtractedDocument& document : documents) {
                if (!document.pdfRenderSession &&
                    QFileInfo(document.sourcePath).suffix().compare(
                        QStringLiteral("pdf"), Qt::CaseInsensitive) == 0) {
                    document.pdfRenderSession =
                        std::make_shared<PdfRenderSession>(document.sourcePath);
                }
            }
            const auto ruleProgress = [&](const QString& pass, int basePercent, int spanPercent) {
                return [&, pass, basePercent, spanPercent](const RuleGenerationProgress& rule) {
                    WorkflowProgress snapshot;
                    snapshot.stage = WorkflowStage::Chunking;
                    snapshot.completedSourceBlocks = documents.size();
                    snapshot.totalSourceBlocks = qMax(1, sources.size());
                    snapshot.rulePass = pass;
                    snapshot.documentName = rule.documentName;
                    snapshot.sectionTitle = rule.sectionTitle;
                    snapshot.sectionIndex = rule.sectionIndex;
                    snapshot.sectionCount = rule.sectionCount;
                    snapshot.questionIndex = rule.questionIndex;
                    snapshot.questionCount = rule.questionCount;
                    snapshot.processedQuestions = rule.processedQuestions;
                    snapshot.acceptedQuestions = rule.acceptedQuestions;
                    snapshot.reviewQuestions = rule.reviewQuestions;
                    const double sectionFraction = rule.sectionCount > 0
                        ? (qMax(0, rule.sectionIndex - 1) +
                           (rule.questionCount > 0
                                ? static_cast<double>(rule.questionIndex) / rule.questionCount
                                : 1.0)) / rule.sectionCount
                        : 0.0;
                    snapshot.percent = qBound(0, basePercent +
                        qRound(spanPercent * sectionFraction), 99);
                    const QString section = rule.sectionTitle.trimmed().isEmpty()
                        ? QStringLiteral("未分套") : rule.sectionTitle.trimmed();
                    snapshot.detail = QStringLiteral("%1 · %2 · 本套第 %3 / %4 题")
                        .arg(rule.documentName, section)
                        .arg(rule.questionIndex)
                        .arg(rule.questionCount);
                    if (rule.questionIndex == 0 || rule.questionIndex == rule.questionCount ||
                        rule.questionIndex % 10 == 0) {
                        QVariantMap fields = diagnostic::memorySnapshot();
                        fields.insert(QStringLiteral("pass"), pass);
                        fields.insert(QStringLiteral("document"), rule.documentName);
                        fields.insert(QStringLiteral("section"), section);
                        fields.insert(QStringLiteral("sectionIndex"), rule.sectionIndex);
                        fields.insert(QStringLiteral("sectionCount"), rule.sectionCount);
                        fields.insert(QStringLiteral("questionIndex"), rule.questionIndex);
                        fields.insert(QStringLiteral("questionCount"), rule.questionCount);
                        fields.insert(QStringLiteral("processedQuestions"), rule.processedQuestions);
                        fields.insert(QStringLiteral("acceptedQuestions"), rule.acceptedQuestions);
                        fields.insert(QStringLiteral("reviewQuestions"), rule.reviewQuestions);
                        fields.insert(QStringLiteral("pageImageCount"), rule.pageImageCount);
                        fields.insert(QStringLiteral("pageImageBytes"), rule.pageImageBytes);
                        fields.insert(QStringLiteral("reviewAssetCount"), rule.reviewAssetCount);
                        fields.insert(QStringLiteral("reviewAssetBytes"), rule.reviewAssetBytes);
                        fields.insert(QStringLiteral("generatedAssetCount"), rule.generatedAssetCount);
                        fields.insert(QStringLiteral("generatedAssetBytes"), rule.generatedAssetBytes);
                        diagnostic::event(QStringLiteral("workflow"),
                                          QStringLiteral("rule-progress"), fields);
                    }
                    publishSnapshot(snapshot);
                };
            };
            if (forcedHasAnswerKey.has_value()) {
                result = RuleBasedBankGenerator{}.generate(
                    documents, *forcedHasAnswerKey,
                    ruleProgress(QStringLiteral("规则整理"), 60, 30));
            } else {
                // 自动模式先用“含答案”语义完整扫描，再按真正成功绑定到具体题目的
                // 答案覆盖率决策。孤立答案记录、无法对齐的答案串和少量误识别不能
                // 以一票否决把整库锁成含答案语义。
                result = RuleBasedBankGenerator{}.generate(
                    documents, true,
                    ruleProgress(QStringLiteral("答案策略探测"), 60, 15));
                const auto [resolvedAnswers, totalQuestions] = resolvedAnswerCoverage(result);
                const bool sparseAnswers = totalQuestions > 0 &&
                    resolvedAnswers * 100 <= totalQuestions * kAutoAnswerCoveragePercent;
                diagnostic::event(QStringLiteral("workflow"), QStringLiteral("answer-policy-auto"),
                    {{QStringLiteral("resolvedAnswers"), resolvedAnswers},
                     {QStringLiteral("totalQuestions"), totalQuestions},
                     {QStringLiteral("thresholdPercent"), kAutoAnswerCoveragePercent},
                     {QStringLiteral("decision"), sparseAnswers
                          ? QStringLiteral("none") : QStringLiteral("included")}});
                if (sparseAnswers) {
                    result = RuleBasedBankGenerator{}.generate(
                        documents, false,
                        ruleProgress(QStringLiteral("按无答案题库重整"), 75, 15));
                    result.warnings.prepend(QStringLiteral(
                        "仅 %1/%2 题识别到答案（不高于 %3%），已按无答案题库整理")
                        .arg(resolvedAnswers).arg(totalQuestions).arg(kAutoAnswerCoveragePercent));
                } else {
                    result.warnings.prepend(QStringLiteral(
                        "已为 %1/%2 题识别到答案，按含答案题库整理")
                        .arg(resolvedAnswers).arg(totalQuestions));
                }
            }
        }
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
            emit self->questionsReady(result);
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
    emit progressChanged({stage, 0, 0, detail});
}

} // namespace quizpane::studio
