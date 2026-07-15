#include "quizpane/studio/generation_workflow.hpp"

#include "quizpane/bank_validator.hpp"
#include "quizpane/diagnostic_logger.hpp"
#include "quizpane/studio/document_extractor.hpp"
#include "quizpane/studio/rule_based_generator.hpp"

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QRegularExpression>
#include <QUuid>

namespace quizpane::studio {
namespace {
const QString kSystemPrompt = QStringLiteral(
    "你是题库结构化助手。只输出一个 JSON 对象，格式为 "
    "{\"materials\":[...],\"questions\":[...]}。共享文章、图表或案例必须只放在 "
    "materials 中，每份材料包含 id、catalogId、title、body；关联子题用 materialId 引用，"
    "不得把材料正文复制到每道题 stem。没有共享材料时返回空 materials 数组。"
    "生成 single_choice、multiple_choice 或 true_false；原文标注多选题或答案有多个选项时必须使用 multiple_choice。每题必须包含 id、catalogId、type、stem、"
    "options、answer、solution；catalogId 固定为 generated；id 和选项 id 只能用小写"
    "字母、数字、点、下划线或连字符；answer.optionIds 必须恰好引用一个现有选项。"
    "忠于原文，不确定时设置 review:{\"needsReview\":true,\"reason\":\"...\","
    "\"confidence\":0.5}，不要编造事实。");

QString stripCodeFence(QString text) {
    text = text.trimmed();
    if (text.startsWith(QStringLiteral("```"))) {
        const int firstNewline = text.indexOf('\n');
        const int lastFence = text.lastIndexOf(QStringLiteral("```"));
        if (firstNewline >= 0 && lastFence > firstNewline)
            text = text.mid(firstNewline + 1, lastFence - firstNewline - 1).trimmed();
    }
    return text;
}

QStringList validationMessages(const QList<BankValidationError>& errors) {
    QStringList messages;
    for (const auto& error : errors) {
        QJsonObject structured{{"message", error.message}, {"questionIndex", error.questionIndex}};
        if (!error.questionId.isEmpty())
            structured.insert("questionId", error.questionId);
        if (!error.materialId.isEmpty())
            structured.insert("materialId", error.materialId);
        messages.append(
            QString::fromUtf8(QJsonDocument(structured).toJson(QJsonDocument::Compact)));
    }
    return messages;
}

QString safeId(QString value, const QString& fallback) {
    value = value.toLower();
    value.replace(QRegularExpression(QStringLiteral("[^a-z0-9._-]")), QStringLiteral("-"));
    while (value.startsWith('-') || value.startsWith('.') || value.startsWith('_'))
        value.remove(0, 1);
    return value.isEmpty() ? fallback : value.left(64);
}
} // namespace

GeneratedBankCandidate parseGeneratedBankCandidate(const QString& rawText, QString* error) {
    if (error)
        error->clear();
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(stripCodeFence(rawText).toUtf8(), &parseError);
    if (!document.isObject()) {
        if (error)
            *error = QStringLiteral("模型输出不是 JSON 对象：%1").arg(parseError.errorString());
        return {};
    }
    const QJsonObject object = document.object();
    if (!object.value("materials").isArray()) {
        if (error)
            *error = QStringLiteral("模型输出缺少 materials 数组");
        return {};
    }
    if (!object.value("questions").isArray()) {
        if (error)
            *error = QStringLiteral("模型输出缺少 questions 数组");
        return {};
    }
    return {object.value("materials").toArray(), object.value("questions").toArray(), {}};
}

GenerationWorkflow::GenerationWorkflow(QNetworkAccessManager* manager, QObject* parent)
    : QObject(parent), client_(manager, this) {
    connect(&client_, &ModelClient::finished, this, &GenerationWorkflow::handleModelResult);
}

void GenerationWorkflow::start(const QStringList& sourcePaths, const ModelSettings& settings,
                               const QString& resumeTaskId) {
    if (active_)
        return;
    diagnostic::event(QStringLiteral("workflow"), QStringLiteral("start"),
        {{QStringLiteral("mode"), QStringLiteral("model")},
         {QStringLiteral("sources"), sourcePaths.size()},
         {QStringLiteral("vendor"), settings.vendorId},
         {QStringLiteral("model"), settings.modelName}});
    if (settings.vendorId == QStringLiteral("anthropic")) {
        failWorkflow(QStringLiteral("Anthropic Messages API 生成暂未实现"));
        return;
    }
    settings_ = settings;
    checkpoint_ = {};
    chunks_.clear();
    currentChunkPosition_ = -1;
    repairing_ = false;
    lastRawOutput_.clear();
    paused_ = false;
    active_ = true;

    QStringList absolutePaths;
    for (const QString& path : sourcePaths)
        absolutePaths.append(QFileInfo(path).absoluteFilePath());
    const QString computedTaskId = store_.taskIdForSources(absolutePaths);
    const QString taskId = resumeTaskId.isEmpty() ? computedTaskId : resumeTaskId;
    QString checkpointError;
    GenerationCheckpoint loaded;
    if (store_.load(taskId, &loaded, &checkpointError)) {
        if (loaded.sourcePaths != absolutePaths) {
            failWorkflow(QStringLiteral("检查点的源文件列表与当前任务不一致"));
            return;
        }
        if (!store_.sourcesUnchanged(loaded, &checkpointError)) {
            failWorkflow(checkpointError + QStringLiteral("。请删除旧任务后重新开始"));
            return;
        }
        checkpoint_ = loaded;
    } else if (!checkpointError.isEmpty()) {
        failWorkflow(checkpointError);
        return;
    } else {
        checkpoint_.taskId = computedTaskId;
        checkpoint_.sourcePaths = absolutePaths;
        if (!store_.createFingerprints(absolutePaths, &checkpoint_.sources, &checkpointError)) {
            failWorkflow(checkpointError);
            return;
        }
    }

    publish(WorkflowStage::Extracting, QStringLiteral("正在提取 TXT/Markdown 文本…"));
    ExtractorRegistry registry;
    Chunker chunker;
    int nextIndex = 0;
    for (const QString& path : absolutePaths) {
        const ExtractedDocument document = registry.extract(path);
        if (!document.error.isEmpty()) {
            failWorkflow(QStringLiteral("%1：%2").arg(QFileInfo(path).fileName(), document.error));
            return;
        }
        if (!document.warnings.isEmpty())
            publish(WorkflowStage::Extracting,
                    QStringLiteral("%1：%2").arg(QFileInfo(path).fileName(),
                                                   document.warnings.join(QStringLiteral("；"))));
        diagnostic::event(QStringLiteral("extractor"), QStringLiteral("success"),
            {{QStringLiteral("file"), QFileInfo(path).fileName()},
             {QStringLiteral("path"), QFileInfo(path).absoluteFilePath()},
             {QStringLiteral("characters"), document.plainText.size()},
             {QStringLiteral("ocr"), document.usedOcr}});
#ifdef QUIZPANE_VERBOSE_DIAGNOSTICS
        diagnostic::payload(QStringLiteral("extractor"), QStringLiteral("content"),
                            QFileInfo(path).fileName(), document.plainText, 64 * 1024);
#endif
        publish(WorkflowStage::Chunking,
                QStringLiteral("正在分段：%1").arg(QFileInfo(path).fileName()));
        const auto fileChunks = chunker.split(path, document.plainText, nextIndex);
        chunks_ += fileChunks;
        for (const auto& chunk : fileChunks)
            nextIndex += chunk.sourceBlockIndices.size();
    }
    if (chunks_.isEmpty()) {
        failWorkflow(QStringLiteral("没有可发送给模型的文本内容"));
        return;
    }
    if (checkpoint_.sourceBlockCount != 0 && checkpoint_.sourceBlockCount != nextIndex) {
        failWorkflow(QStringLiteral("任务结构已升级，请重新开始"));
        return;
    }
    checkpoint_.sourceBlockCount = nextIndex;
    processNextChunk();
}

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
    checkpoint_ = {};
    chunks_.clear();
    currentChunkPosition_ = -1;
    repairing_ = false;
    lastRawOutput_.clear();
    paused_ = false;
    active_ = true;

    QList<ExtractedDocument> documents;
    ExtractorRegistry registry;
    int completed = 0;
    for (const SourceMaterialGroup& source : sources) {
        const QString path = source.questionPath;
        publish(WorkflowStage::Extracting,
                QStringLiteral("正在本地提取：%1").arg(QFileInfo(path).fileName()));
        ExtractedDocument document = registry.extract(QFileInfo(path).absoluteFilePath());
        if (!document.error.isEmpty()) {
            failWorkflow(QStringLiteral("%1：%2").arg(QFileInfo(path).fileName(), document.error));
            return;
        }
        if (!document.warnings.isEmpty())
            publish(WorkflowStage::Extracting,
                    QStringLiteral("%1：%2").arg(QFileInfo(path).fileName(),
                                                   document.warnings.join(QStringLiteral("；"))));
        if (!source.answerPath.isEmpty()) {
            const ExtractedDocument answers = registry.extract(
                QFileInfo(source.answerPath).absoluteFilePath());
            if (!answers.error.isEmpty()) {
                failWorkflow(QStringLiteral("%1：%2")
                    .arg(QFileInfo(source.answerPath).fileName(), answers.error));
                return;
            }
            // 规则生成器本来就支持“答案汇总”区。把独立答案文件接入该区，既不会
            // 伪造题目，也能按题号将答案和解析归回题目文件。
            document.plainText += QStringLiteral("\n\n答案及解析\n") + answers.plainText;
        }
        diagnostic::event(QStringLiteral("extractor"), QStringLiteral("success"),
            {{QStringLiteral("file"), QFileInfo(path).fileName()},
             {QStringLiteral("path"), QFileInfo(path).absoluteFilePath()},
             {QStringLiteral("characters"), document.plainText.size()},
             {QStringLiteral("ocr"), document.usedOcr}});
#ifdef QUIZPANE_VERBOSE_DIAGNOSTICS
        diagnostic::payload(QStringLiteral("extractor"), QStringLiteral("content"),
                            QFileInfo(path).fileName(), document.plainText, 64 * 1024);
#endif
        documents.append(document);
        ++completed;
        checkpoint_.sourceBlockCount = sources.size();
        checkpoint_.completedSourceBlocks.append(completed - 1);
    }

    publish(WorkflowStage::Chunking, QStringLiteral("正在按题号、选项、答案和材料规则解析"));
    const RuleBasedGenerationResult result = RuleBasedBankGenerator{}.generate(documents);
    if (result.questions.isEmpty() && result.needsReviewQuestions.isEmpty()) {
        active_ = false;
        const QString detail = result.warnings.isEmpty()
                                   ? QStringLiteral("规则引擎没有识别到题目")
                                   : result.warnings.join(QStringLiteral("；"));
        publish(WorkflowStage::Failed, detail);
        emit failed(detail);
        return;
    }

    active_ = false;
    const QString detail = QStringLiteral("规则解析完成：%1 道可直接使用，%2 道待复核")
                               .arg(result.questions.size())
                               .arg(result.needsReviewQuestions.size());
    publish(WorkflowStage::Done, detail);
    emit questionsReady({result.materials, result.questions,
                         result.needsReviewQuestions, result.warnings});
    emit finished();
}

void GenerationWorkflow::pause() {
    if (!active_)
        return;
    paused_ = true;
    client_.cancel();
    QString ignored;
    store_.save(checkpoint_, &ignored);
    publish(WorkflowStage::Idle, QStringLiteral("任务已暂停，检查点已保留"));
}

void GenerationWorkflow::cancel() {
    client_.cancel();
    active_ = false;
    paused_ = false;
    if (!checkpoint_.taskId.isEmpty())
        store_.clear(checkpoint_.taskId);
}

void GenerationWorkflow::processNextChunk() {
    if (!active_ || paused_)
        return;
    QSet<int> completed(checkpoint_.completedSourceBlocks.begin(),
                        checkpoint_.completedSourceBlocks.end());
    ++currentChunkPosition_;
    while (currentChunkPosition_ < chunks_.size()) {
        bool allCompleted = true;
        for (int index : chunks_[currentChunkPosition_].sourceBlockIndices)
            allCompleted = allCompleted && completed.contains(index);
        if (!allCompleted)
            break;
        ++currentChunkPosition_;
    }
    if (currentChunkPosition_ >= chunks_.size()) {
        active_ = false;
        publish(WorkflowStage::Done, QStringLiteral("全部分块已生成并校验"));
        emit questionsReady(
            {checkpoint_.materials, checkpoint_.questions, checkpoint_.needsReviewQuestions});
        emit finished();
        return;
    }
    repairing_ = false;
    requestChunk(false);
}

void GenerationWorkflow::requestChunk(bool repair, const QStringList& errors) {
    const TextChunk& chunk = chunks_.at(currentChunkPosition_);
    publish(repair ? WorkflowStage::Repairing : WorkflowStage::Generating,
            QStringLiteral("%1分块 %2 / %3：%4")
                .arg(repair ? QStringLiteral("正在修复") : QStringLiteral("正在生成"))
                .arg(chunk.index + 1)
                .arg(chunks_.size())
                .arg(QFileInfo(chunk.sourcePath).fileName()));
    QString content;
    if (repair) {
        content = QStringLiteral("上一轮输出未通过规则校验。结构化错误：\n%1\n\n"
                                 "上一轮完整输出：\n%2\n\n原文块：\n%3\n\n"
                                 "请重新输出完整且合法的 materials + questions JSON，"
                                 "修复材料及其全部关联子题，不要只返回单题。")
                      .arg(errors.join(QChar('\n')), lastRawOutput_, chunk.text);
    } else {
        content = QStringLiteral("请从以下原文提取可回答的题目。若原文不足以形成可靠题目，"
                                 "返回空 materials 和 questions 数组。\n\n来源：%1\n\n%2")
                      .arg(QFileInfo(chunk.sourcePath).fileName(), chunk.text);
    }
    client_.generate(settings_, {kSystemPrompt, content, settings_.modelName});
}

void GenerationWorkflow::handleModelResult(const GenerationResult& result) {
    if (!active_ || paused_)
        return;
    if (!result.ok) {
        failWorkflow(QStringLiteral("模型调用失败：%1").arg(result.error));
        return;
    }
#ifdef QUIZPANE_VERBOSE_DIAGNOSTICS
    diagnostic::payload(QStringLiteral("model"), QStringLiteral("response"),
                        QStringLiteral("raw-json"), result.rawText, 64 * 1024);
#endif
    checkpoint_.inputTokens += result.promptTokens;
    checkpoint_.outputTokens += result.completionTokens;
    lastRawOutput_ = result.rawText;
    QString parseError;
    GeneratedBankCandidate candidate = parseGeneratedBankCandidate(result.rawText, &parseError);
    if (!parseError.isEmpty()) {
        if (!repairing_) {
            repairing_ = true;
            requestChunk(true, {parseError});
            return;
        }
        failWorkflow(QStringLiteral("模型修复后仍无法解析：%1").arg(parseError));
        return;
    }
    if (candidate.questions.isEmpty() && candidate.materials.isEmpty()) {
        completeChunk({});
        return;
    }
    publish(WorkflowStage::Validating,
            QStringLiteral("正在校验分块 %1 的题目结构").arg(currentChunkPosition_ + 1));
    const auto errors = validateBankDetailed(bankForCandidate(candidate));
    if (!errors.isEmpty()) {
        if (!repairing_) {
            repairing_ = true;
            requestChunk(true, validationMessages(errors));
            return;
        }
        const QString reason = validationMessages(errors).join(QStringLiteral("；"));
        QJsonArray review;
        for (const auto& value : candidate.questions) {
            if (!value.isObject())
                continue;
            QJsonObject question = value.toObject();
            question.insert("review", QJsonObject{{"needsReview", true},
                                                  {"reason", reason.left(1000)},
                                                  {"confidence", 0.0}});
            review.append(question);
        }
        candidate.questions = {};
        candidate.needsReviewQuestions = review;
        completeChunk(candidate);
        return;
    }
    QJsonArray normal;
    QJsonArray review;
    for (const auto& value : candidate.questions) {
        const QJsonObject question = value.toObject();
        if (question.value("review").toObject().value("needsReview").toBool())
            review.append(question);
        else
            normal.append(question);
    }
    candidate.questions = normal;
    candidate.needsReviewQuestions = review;
    completeChunk(candidate);
}

QJsonObject GenerationWorkflow::bankForCandidate(const GeneratedBankCandidate& candidate) const {
    return {
        {"schemaVersion", 2},
        {"title", "Generated"},
        {"catalogs", QJsonArray{QJsonObject{{"id", "generated"},
                                            {"title", "自动生成"},
                                            {"practice", QJsonObject{{"mode", "sequential"}}}}}},
        {"materials", candidate.materials},
        {"questions", candidate.questions}};
}

void GenerationWorkflow::completeChunk(GeneratedBankCandidate candidate) {
    const TextChunk& chunk = chunks_.at(currentChunkPosition_);
    const int blockIndex =
        chunk.sourceBlockIndices.isEmpty() ? 0 : chunk.sourceBlockIndices.first();
    QHash<QString, QString> renamedInChunk;
    int materialOrdinal = 0;
    for (const auto& value : candidate.materials) {
        if (!value.isObject())
            continue;
        QJsonObject material = value.toObject();
        const QString originalId = material.value("id").toString();
        const QString renameKey = QStringLiteral("%1:%2").arg(blockIndex).arg(originalId);
        QString stableId = checkpoint_.materialIdRenames.value(renameKey).toString();
        if (stableId.isEmpty()) {
            stableId = QStringLiteral("b%1-m%2-%3")
                           .arg(blockIndex)
                           .arg(++materialOrdinal)
                           .arg(safeId(originalId, QStringLiteral("material")));
            stableId = stableId.left(96);
            checkpoint_.materialIdRenames.insert(renameKey, stableId);
        }
        renamedInChunk.insert(originalId, stableId);
        material.insert("id", stableId);
        material.insert("catalogId", QStringLiteral("generated"));
        checkpoint_.materials.append(material);
    }
    const auto appendPrepared = [this, blockIndex, &renamedInChunk](const QJsonArray& source,
                                                                    QJsonArray* target) {
        int ordinal = 0;
        for (const auto& value : source) {
            if (!value.isObject())
                continue;
            QJsonObject question = value.toObject();
            const QString originalId =
                safeId(question.value("id").toString(), QStringLiteral("question"));
            const QString prefix = QStringLiteral("b%1-q%2-").arg(blockIndex).arg(++ordinal);
            question.insert("id", prefix + originalId.left(96 - prefix.size()));
            question.insert("catalogId", QStringLiteral("generated"));
            const QString originalMaterialId = question.value("materialId").toString();
            if (!originalMaterialId.isEmpty() && renamedInChunk.contains(originalMaterialId)) {
                const QString materialId = renamedInChunk.value(originalMaterialId);
                question.insert("materialId", materialId);
                QJsonArray ids = checkpoint_.materialQuestionIds.value(materialId).toArray();
                ids.append(question.value("id").toString());
                checkpoint_.materialQuestionIds.insert(materialId, ids);
            }
            target->append(question);
        }
    };
    appendPrepared(candidate.questions, &checkpoint_.questions);
    appendPrepared(candidate.needsReviewQuestions, &checkpoint_.needsReviewQuestions);
    for (int index : chunk.sourceBlockIndices)
        if (!checkpoint_.completedSourceBlocks.contains(index))
            checkpoint_.completedSourceBlocks.append(index);
    QString error;
    if (!store_.save(checkpoint_, &error)) {
        failWorkflow(QStringLiteral("无法保存生成检查点：%1").arg(error));
        return;
    }
    processNextChunk();
}

void GenerationWorkflow::publish(WorkflowStage stage, const QString& detail) {
    diagnostic::event(QStringLiteral("workflow"), QStringLiteral("progress"),
        {{QStringLiteral("stage"), static_cast<int>(stage)},
         {QStringLiteral("completed"), checkpoint_.completedSourceBlocks.size()},
         {QStringLiteral("total"), checkpoint_.sourceBlockCount},
         {QStringLiteral("inputTokens"), checkpoint_.inputTokens},
         {QStringLiteral("outputTokens"), checkpoint_.outputTokens},
         {QStringLiteral("detail"), detail}});
    emit progressChanged({stage, static_cast<int>(checkpoint_.completedSourceBlocks.size()),
                          checkpoint_.sourceBlockCount, checkpoint_.inputTokens,
                          checkpoint_.outputTokens, detail});
}

void GenerationWorkflow::failWorkflow(const QString& error) {
    diagnostic::event(QStringLiteral("workflow"), QStringLiteral("failed"),
        {{QStringLiteral("error"), error},
         {QStringLiteral("chunk"), currentChunkPosition_}});
    client_.cancel();
    active_ = false;
    QString ignored;
    if (!checkpoint_.taskId.isEmpty())
        store_.save(checkpoint_, &ignored);
    publish(WorkflowStage::Failed, error);
    emit failed(error);
}

} // namespace quizpane::studio
