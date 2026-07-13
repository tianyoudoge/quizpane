#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonObject>
#include <QTemporaryDir>

#include "quizpane/draft_store.hpp"

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir temp;
    if (!temp.isValid()) return 1;
    qputenv("QUIZPANE_DRAFTS_ROOT", temp.path().toUtf8());

    quizpane::DraftStore store;
    quizpane::DraftSnapshot source;
    source.providerId = QStringLiteral("org.quizpane.demo");
    source.attemptId = QStringLiteral("attempt-42");
    source.title = QStringLiteral("测试练习");
    source.questions = QJsonArray{QJsonObject{{"id", "q1"}},
                                  QJsonObject{{"id", "q2"}}};
    source.answers = QVector<int>{2, -1};
    source.currentQuestionIndex = 1;
    QString error;
    if (!store.save(source, &error)) return 2;
    if (!QFileInfo(store.pathForProvider(source.providerId)).isFile()) return 3;
    quizpane::DraftSnapshot restored;
    if (!store.load(source.providerId, &restored, &error)) return 4;
    if (restored.attemptId != source.attemptId || restored.questions.size() != 2 ||
        restored.answers != source.answers || restored.currentQuestionIndex != 1) return 5;
    if (!store.clear(source.providerId, &error)) return 6;
    if (store.load(source.providerId, &restored, &error)) return 7;
    return 0;
}
