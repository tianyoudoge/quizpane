#include "quizpane/studio/checkpoint_store.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    if (!directory.isValid())
        return 1;
    qputenv("QUIZPANE_GENERATION_TASKS_ROOT", directory.path().toUtf8());
    const QString source = directory.filePath(QStringLiteral("source.txt"));
    QFile file(source);
    if (!file.open(QIODevice::WriteOnly))
        return 2;
    file.write("first");
    file.close();

    quizpane::studio::CheckpointStore store;
    quizpane::studio::GenerationCheckpoint value;
    value.sourcePaths = {source};
    value.taskId = store.taskIdForSources(value.sourcePaths);
    QString error;
    if (!store.createFingerprints(value.sourcePaths, &value.sources, &error))
        return 3;
    value.sourceBlockCount = 3;
    value.completedSourceBlocks = {0, 2};
    value.materials = {QJsonObject{{"id", "b0-m1-reading"}}};
    value.questions = {QJsonObject{{"id", "q1"}}};
    value.materialIdRenames = QJsonObject{{"0:reading", "b0-m1-reading"}};
    value.materialQuestionIds = QJsonObject{{"b0-m1-reading", QJsonArray{"q1"}}};
    value.inputTokens = 12;
    value.outputTokens = 7;
    if (!store.save(value, &error))
        return 4;
    quizpane::studio::GenerationCheckpoint loaded;
    if (!store.load(value.taskId, &loaded, &error))
        return 5;
    if (loaded.completedSourceBlocks != value.completedSourceBlocks || loaded.inputTokens != 12 ||
        loaded.materials.size() != 1 || loaded.questions.size() != 1 ||
        loaded.materialIdRenames != value.materialIdRenames ||
        !store.sourcesUnchanged(loaded, &error))
        return 6;

    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return 7;
    file.write("changed");
    file.close();
    error.clear();
    if (store.sourcesUnchanged(loaded, &error) || !error.contains(QStringLiteral("已修改")))
        return 8;

    // v1 不猜测材料关系，也不静默迁移。
    QFile legacy(store.pathForTask(QStringLiteral("legacy")));
    if (!legacy.open(QIODevice::WriteOnly))
        return 9;
    legacy.write(QJsonDocument(QJsonObject{{"version", 1},
                                           {"taskId", "legacy"},
                                           {"sourcePaths", QJsonArray{source}},
                                           {"sources", QJsonArray{}},
                                           {"completedChunks", QJsonArray{0}},
                                           {"questions", QJsonArray{}}})
                     .toJson(QJsonDocument::Compact));
    legacy.close();
    error.clear();
    if (store.load(QStringLiteral("legacy"), &loaded, &error) ||
        !error.contains(QStringLiteral("任务结构已升级，请重新开始")))
        return 10;
    return 0;
}
