#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
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
    source.questions = QJsonArray{
        QJsonObject{{"id", "q1"}, {"materialId", "material-001"}},
        QJsonObject{{"id", "q2"}}};
    source.materials = QJsonArray{QJsonObject{{"id", "material-001"},
        {"title", "阅读材料一"}, {"contentHtml", "<p>材料正文</p>"}}};
    source.answers = QVector<int>{2, -1};
    source.currentQuestionIndex = 1;
    QString error;
    if (!store.save(source, &error)) return 2;
    if (!QFileInfo(store.pathForAttempt(source.providerId, source.attemptId)).isFile()) return 3;
    quizpane::DraftSnapshot restored;
    if (!store.load(source.providerId, &restored, &error)) return 4;
    if (restored.attemptId != source.attemptId || restored.questions.size() != 2 ||
        restored.answers != source.answers || restored.currentQuestionIndex != 1) return 5;
    // 恢复到题组中间某一题时，材料卡片必须能立即拿到内容，不依赖重新请求 Provider。
    if (restored.materials.size() != 1) return 6;
    const QJsonObject restoredMaterial = restored.materials.first().toObject();
    if (restoredMaterial.value("id").toString() != QStringLiteral("material-001") ||
        restoredMaterial.value("contentHtml").toString() != QStringLiteral("<p>材料正文</p>")) return 7;
    quizpane::DraftSnapshot second = source;
    second.attemptId = QStringLiteral("attempt-43");
    second.title = QStringLiteral("另一套练习");
    if (!store.save(second, &error) || store.list(source.providerId, &error).size() != 2) return 8;
    if (!store.clearAttempt(source.providerId, second.attemptId, &error) ||
        store.list(source.providerId, &error).size() != 1) return 9;
    if (!store.clear(source.providerId, &error)) return 10;
    if (store.load(source.providerId, &restored, &error)) return 11;
    return 0;
}
