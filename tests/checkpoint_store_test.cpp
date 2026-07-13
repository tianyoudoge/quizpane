#include "quizpane/studio/checkpoint_store.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    if (!directory.isValid()) return 1;
    qputenv("QUIZPANE_GENERATION_TASKS_ROOT", directory.path().toUtf8());
    const QString source = directory.filePath(QStringLiteral("source.txt"));
    QFile file(source);
    if (!file.open(QIODevice::WriteOnly)) return 2;
    file.write("first"); file.close();

    quizpane::studio::CheckpointStore store;
    quizpane::studio::GenerationCheckpoint value;
    value.sourcePaths = {source};
    value.taskId = store.taskIdForSources(value.sourcePaths);
    QString error;
    if (!store.createFingerprints(value.sourcePaths, &value.sources, &error)) return 3;
    value.completedChunks = {0, 2};
    value.questions = {QJsonObject{{"id", "q1"}}};
    value.inputTokens = 12; value.outputTokens = 7;
    if (!store.save(value, &error)) return 4;
    quizpane::studio::GenerationCheckpoint loaded;
    if (!store.load(value.taskId, &loaded, &error)) return 5;
    if (loaded.completedChunks != value.completedChunks || loaded.inputTokens != 12 ||
        loaded.questions.size() != 1 || !store.sourcesUnchanged(loaded, &error)) return 6;

    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 7;
    file.write("changed"); file.close();
    error.clear();
    if (store.sourcesUnchanged(loaded, &error) || !error.contains(QStringLiteral("已修改"))) return 8;
    return 0;
}
