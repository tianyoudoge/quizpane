#include "quizpane/studio/generation_workflow.hpp"

#include "quizpane/bank_validator.hpp"
#include "quizpane/studio/document_extractor.hpp"

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QRegularExpression>
#include <QUuid>

namespace quizpane::studio {
namespace {
const QString kSystemPrompt = QStringLiteral(
    "你是题库结构化助手。只输出一个 JSON 对象，格式为 {\"questions\":[...]}。"
    "只生成 single_choice 或 true_false。每题必须包含 id、catalogId、type、stem、"
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
    for (const auto& error : errors) messages.append(error.message);
    return messages;
}
}  // namespace

GenerationWorkflow::GenerationWorkflow(QNetworkAccessManager* manager, QObject* parent)
    : QObject(parent), client_(manager, this) {
    connect(&client_, &ModelClient::finished, this, &GenerationWorkflow::handleModelResult);
}

void GenerationWorkflow::start(const QStringList& sourcePaths, const ModelSettings& settings,
                               const QString& resumeTaskId) {
    if (active_) return;
    if (settings.vendorId == QStringLiteral("anthropic")) {
        failWorkflow(QStringLiteral("Anthropic Messages API 生成暂未实现"));
        return;
    }
    settings_ = settings;
    checkpoint_ = {};
    chunks_.clear();
    currentChunkPosition_ = -1;
    repairing_ = false;
    paused_ = false;
    active_ = true;

    QStringList absolutePaths;
    for (const QString& path : sourcePaths) absolutePaths.append(QFileInfo(path).absoluteFilePath());
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
        publish(WorkflowStage::Chunking, QStringLiteral("正在分段：%1").arg(QFileInfo(path).fileName()));
        const auto fileChunks = chunker.split(path, document.plainText, nextIndex);
        chunks_ += fileChunks;
        nextIndex += fileChunks.size();
    }
    if (chunks_.isEmpty()) {
        failWorkflow(QStringLiteral("没有可发送给模型的文本内容"));
        return;
    }
    processNextChunk();
}

void GenerationWorkflow::pause() {
    if (!active_) return;
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
    if (!checkpoint_.taskId.isEmpty()) store_.clear(checkpoint_.taskId);
}

void GenerationWorkflow::processNextChunk() {
    if (!active_ || paused_) return;
    QSet<int> completed(checkpoint_.completedChunks.begin(), checkpoint_.completedChunks.end());
    ++currentChunkPosition_;
    while (currentChunkPosition_ < chunks_.size() && completed.contains(chunks_[currentChunkPosition_].index))
        ++currentChunkPosition_;
    if (currentChunkPosition_ >= chunks_.size()) {
        active_ = false;
        publish(WorkflowStage::Done, QStringLiteral("全部分块已生成并校验"));
        emit questionsReady(checkpoint_.questions, checkpoint_.needsReviewQuestions);
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
            .arg(chunk.index + 1).arg(chunks_.size()).arg(QFileInfo(chunk.sourcePath).fileName()));
    QString content;
    if (repair) {
        content = QStringLiteral("上一轮输出未通过规则校验。错误：\n- %1\n\n原文：\n%2\n\n"
                                 "请重新输出完整且合法的 questions JSON。")
                      .arg(errors.join(QStringLiteral("\n- ")), chunk.text);
    } else {
        content = QStringLiteral("请从以下原文提取可回答的题目。若原文不足以形成可靠题目，"
                                 "返回空 questions 数组。\n\n来源：%1\n\n%2")
                      .arg(QFileInfo(chunk.sourcePath).fileName(), chunk.text);
    }
    client_.generate(settings_, {kSystemPrompt, content, settings_.modelName});
}

void GenerationWorkflow::handleModelResult(const GenerationResult& result) {
    if (!active_ || paused_) return;
    if (!result.ok) {
        failWorkflow(QStringLiteral("模型调用失败：%1").arg(result.error));
        return;
    }
    checkpoint_.inputTokens += result.promptTokens;
    checkpoint_.outputTokens += result.completionTokens;
    QString parseError;
    const QJsonArray questions = parseQuestions(result.rawText, &parseError);
    if (!parseError.isEmpty()) {
        if (!repairing_) {
            repairing_ = true;
            requestChunk(true, {parseError});
            return;
        }
        completeChunk({}, {});
        return;
    }
    if (questions.isEmpty()) {
        completeChunk({}, {});
        return;
    }
    publish(WorkflowStage::Validating, QStringLiteral("正在校验分块 %1 的题目结构").arg(currentChunkPosition_ + 1));
    const auto errors = validateBankDetailed(bankForQuestions(questions));
    if (!errors.isEmpty()) {
        if (!repairing_) {
            repairing_ = true;
            requestChunk(true, validationMessages(errors));
            return;
        }
        completeChunk({}, {});
        return;
    }
    QJsonArray normal;
    QJsonArray review;
    for (const auto& value : questions) {
        const QJsonObject question = value.toObject();
        if (question.value("review").toObject().value("needsReview").toBool()) review.append(question);
        else normal.append(question);
    }
    completeChunk(normal, review);
}

QJsonArray GenerationWorkflow::parseQuestions(const QString& rawText, QString* error) const {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(stripCodeFence(rawText).toUtf8(), &parseError);
    if (!document.isObject()) {
        if (error) *error = QStringLiteral("模型输出不是 JSON 对象：%1").arg(parseError.errorString());
        return {};
    }
    const QJsonValue value = document.object().value("questions");
    if (!value.isArray()) {
        if (error) *error = QStringLiteral("模型输出缺少 questions 数组");
        return {};
    }
    return value.toArray();
}

QJsonObject GenerationWorkflow::bankForQuestions(const QJsonArray& questions) const {
    return {{"schemaVersion", 1}, {"title", "Generated"},
        {"catalogs", QJsonArray{QJsonObject{{"id", "generated"}, {"title", "自动生成"},
            {"practice", QJsonObject{{"mode", "sequential"}}}}}}, {"questions", questions}};
}

void GenerationWorkflow::completeChunk(const QJsonArray& valid, const QJsonArray& review) {
    const int chunkIndex = chunks_.at(currentChunkPosition_).index;
    const auto appendPrepared = [chunkIndex](const QJsonArray& source, QJsonArray* target) {
        int ordinal = 0;
        for (const auto& value : source) {
            QJsonObject question = value.toObject();
            QString originalId = question.value("id").toString().toLower();
            originalId.replace(QRegularExpression(QStringLiteral("[^a-z0-9._-]")), QStringLiteral("-"));
            if (originalId.isEmpty()) originalId = QStringLiteral("question");
            const QString prefix = QStringLiteral("c%1-%2-").arg(chunkIndex).arg(++ordinal);
            question.insert("id", prefix + originalId.left(96 - prefix.size()));
            question.insert("catalogId", QStringLiteral("generated"));
            target->append(question);
        }
    };
    appendPrepared(valid, &checkpoint_.questions);
    appendPrepared(review, &checkpoint_.needsReviewQuestions);
    checkpoint_.completedChunks.append(chunks_.at(currentChunkPosition_).index);
    QString error;
    if (!store_.save(checkpoint_, &error)) {
        failWorkflow(QStringLiteral("无法保存生成检查点：%1").arg(error));
        return;
    }
    processNextChunk();
}

void GenerationWorkflow::publish(WorkflowStage stage, const QString& detail) {
    emit progressChanged({stage, static_cast<int>(checkpoint_.completedChunks.size()),
                          static_cast<int>(chunks_.size()),
                          checkpoint_.inputTokens, checkpoint_.outputTokens, detail});
}

void GenerationWorkflow::failWorkflow(const QString& error) {
    client_.cancel();
    active_ = false;
    QString ignored;
    if (!checkpoint_.taskId.isEmpty()) store_.save(checkpoint_, &ignored);
    publish(WorkflowStage::Failed, error);
    emit failed(error);
}

}  // namespace quizpane::studio
