#include "quizpane/studio/checkpoint_store.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

namespace quizpane::studio {
namespace {
bool fail(QString* error, const QString& message) {
    if (error)
        *error = message;
    return false;
}

QString fileDigest(const QString& path, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        fail(error, QStringLiteral("无法读取源文件 %1：%2").arg(path, file.errorString()));
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        fail(error, QStringLiteral("无法计算源文件摘要：%1").arg(path));
        return {};
    }
    return QString::fromLatin1(hash.result().toHex());
}
} // namespace

QString CheckpointStore::taskIdForSources(const QStringList& sourcePaths) const {
    QStringList normalized;
    for (const QString& path : sourcePaths)
        normalized.append(QFileInfo(path).absoluteFilePath());
    normalized.sort(Qt::CaseSensitive);
    return QString::fromLatin1(
        QCryptographicHash::hash(normalized.join(QChar::Null).toUtf8(), QCryptographicHash::Sha256)
            .toHex());
}

QString CheckpointStore::pathForTask(const QString& taskId) const {
    QString root = qEnvironmentVariable("QUIZPANE_GENERATION_TASKS_ROOT");
    if (root.isEmpty()) {
        root = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                   .filePath(QStringLiteral("generation-tasks"));
    }
    return QDir(root).filePath(taskId + QStringLiteral(".json"));
}

bool CheckpointStore::createFingerprints(const QStringList& sourcePaths,
                                         QList<SourceFingerprint>* fingerprints,
                                         QString* error) const {
    if (!fingerprints || sourcePaths.isEmpty())
        return fail(error, QStringLiteral("没有可生成摘要的源文件"));
    QList<SourceFingerprint> result;
    for (const QString& path : sourcePaths) {
        QString digestError;
        const QString digest = fileDigest(path, &digestError);
        if (digest.isEmpty())
            return fail(error, digestError);
        result.append({QFileInfo(path).absoluteFilePath(), digest});
    }
    *fingerprints = result;
    return true;
}

bool CheckpointStore::save(const GenerationCheckpoint& checkpoint, QString* error) const {
    if (checkpoint.taskId.isEmpty() || checkpoint.sourcePaths.isEmpty() ||
        checkpoint.sources.size() != checkpoint.sourcePaths.size() ||
        checkpoint.sourceBlockCount < 0)
        return fail(error, QStringLiteral("生成检查点缺少任务标识或源文件"));
    QJsonArray sources;
    for (const auto& source : checkpoint.sources)
        sources.append(QJsonObject{{"path", source.path}, {"sha256", source.sha256}});
    QJsonArray completed;
    QSet<int> completedIndices;
    for (int index : checkpoint.completedSourceBlocks) {
        if (index < 0 || index >= checkpoint.sourceBlockCount || completedIndices.contains(index))
            return fail(error, QStringLiteral("生成检查点包含无效的源块进度"));
        completedIndices.insert(index);
        completed.append(index);
    }
    QJsonArray paths;
    for (const QString& path : checkpoint.sourcePaths)
        paths.append(path);
    const QJsonObject object{{"version", 2},
                             {"taskId", checkpoint.taskId},
                             {"sourcePaths", paths},
                             {"sources", sources},
                             {"sourceBlockCount", checkpoint.sourceBlockCount},
                             {"completedSourceBlocks", completed},
                             {"materials", checkpoint.materials},
                             {"questions", checkpoint.questions},
                             {"needsReviewQuestions", checkpoint.needsReviewQuestions},
                             {"materialIdRenames", checkpoint.materialIdRenames},
                             {"materialQuestionIds", checkpoint.materialQuestionIds},
                             {"inputTokens", checkpoint.inputTokens},
                             {"outputTokens", checkpoint.outputTokens}};
    const QString path = pathForTask(checkpoint.taskId);
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return fail(error, QStringLiteral("无法创建生成任务目录"));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return fail(error, file.errorString());
    file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    if (!file.commit())
        return fail(error, file.errorString());
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

bool CheckpointStore::load(const QString& taskId, GenerationCheckpoint* checkpoint,
                           QString* error) const {
    if (!checkpoint || taskId.isEmpty())
        return false;
    QFile file(pathForTask(taskId));
    if (!file.exists())
        return false;
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, file.errorString());
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!document.isObject())
        return fail(error,
                    QStringLiteral("生成检查点 JSON 无效：%1").arg(parseError.errorString()));
    const QJsonObject object = document.object();
    if (object.value("version").toInt() != 2)
        return fail(error, QStringLiteral("任务结构已升级，请重新开始"));
    if (object.value("taskId").toString() != taskId)
        return fail(error, QStringLiteral("生成检查点任务标识不匹配"));
    if (!object.value("sourcePaths").isArray() || !object.value("sources").isArray() ||
        !object.value("sourceBlockCount").isDouble() ||
        !object.value("completedSourceBlocks").isArray() || !object.value("materials").isArray() ||
        !object.value("questions").isArray() || !object.value("needsReviewQuestions").isArray() ||
        !object.value("materialIdRenames").isObject() ||
        !object.value("materialQuestionIds").isObject() ||
        !object.value("inputTokens").isDouble() || !object.value("outputTokens").isDouble()) {
        return fail(error, QStringLiteral("任务结构已升级，请重新开始"));
    }
    GenerationCheckpoint result;
    result.taskId = taskId;
    for (const auto& value : object.value("sourcePaths").toArray())
        result.sourcePaths.append(value.toString());
    for (const auto& value : object.value("sources").toArray()) {
        const auto source = value.toObject();
        result.sources.append({source.value("path").toString(), source.value("sha256").toString()});
    }
    const double blockCountValue = object.value("sourceBlockCount").toDouble(-1);
    result.sourceBlockCount = object.value("sourceBlockCount").toInt(-1);
    if (blockCountValue != result.sourceBlockCount || result.sourceBlockCount < 0)
        return fail(error, QStringLiteral("生成检查点内容不完整"));
    QSet<int> completedBlocks;
    for (const auto& value : object.value("completedSourceBlocks").toArray()) {
        const int index = value.toInt(-1);
        if (!value.isDouble() || value.toDouble() != index || index < 0 ||
            index >= result.sourceBlockCount || completedBlocks.contains(index))
            return fail(error, QStringLiteral("生成检查点内容不完整"));
        completedBlocks.insert(index);
        result.completedSourceBlocks.append(index);
    }
    result.materials = object.value("materials").toArray();
    result.questions = object.value("questions").toArray();
    result.needsReviewQuestions = object.value("needsReviewQuestions").toArray();
    result.materialIdRenames = object.value("materialIdRenames").toObject();
    result.materialQuestionIds = object.value("materialQuestionIds").toObject();
    result.inputTokens = object.value("inputTokens").toVariant().toLongLong();
    result.outputTokens = object.value("outputTokens").toVariant().toLongLong();
    bool sourceMetadataValid =
        !result.sourcePaths.isEmpty() && result.sources.size() == result.sourcePaths.size();
    for (const QString& path : result.sourcePaths)
        sourceMetadataValid = sourceMetadataValid && !path.isEmpty();
    for (const auto& source : result.sources)
        sourceMetadataValid =
            sourceMetadataValid && !source.path.isEmpty() && !source.sha256.isEmpty();
    bool relationsValid = true;
    for (auto it = result.materialIdRenames.constBegin(); it != result.materialIdRenames.constEnd();
         ++it)
        relationsValid =
            relationsValid && it.value().isString() && !it.value().toString().isEmpty();
    for (auto it = result.materialQuestionIds.constBegin();
         it != result.materialQuestionIds.constEnd(); ++it) {
        relationsValid = relationsValid && it.value().isArray();
        for (const auto& questionId : it.value().toArray())
            relationsValid =
                relationsValid && questionId.isString() && !questionId.toString().isEmpty();
    }
    if (!sourceMetadataValid || !relationsValid || result.inputTokens < 0 ||
        result.outputTokens < 0)
        return fail(error, QStringLiteral("生成检查点内容不完整"));
    *checkpoint = result;
    return true;
}

bool CheckpointStore::sourcesUnchanged(const GenerationCheckpoint& checkpoint,
                                       QString* error) const {
    for (const auto& source : checkpoint.sources) {
        QString digestError;
        const QString digest = fileDigest(source.path, &digestError);
        if (digest.isEmpty())
            return fail(error, digestError);
        if (digest != source.sha256)
            return fail(error,
                        QStringLiteral("源文件已修改，不能复用旧检查点：%1").arg(source.path));
    }
    return true;
}

bool CheckpointStore::clear(const QString& taskId, QString* error) const {
    const QString path = pathForTask(taskId);
    if (!QFile::exists(path) || QFile::remove(path))
        return true;
    return fail(error, QStringLiteral("无法删除生成检查点"));
}

} // namespace quizpane::studio
