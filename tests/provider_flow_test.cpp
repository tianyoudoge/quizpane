#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>

#include "quizpane/provider_loader.hpp"

QJsonObject call(quizpane::ProviderLoader& loader, const QString& id,
                 const QString& method, const QJsonObject& params = {}) {
    QEventLoop loop;
    QJsonObject response;
    const auto connection = QObject::connect(
        &loader, &quizpane::ProviderLoader::responseReceived, &loop,
        [&](const QJsonObject& candidate) {
            if (candidate.value("id").toString() != id) return;
            response = candidate;
            loop.quit();
        });
    QString error;
    if (!loader.request({{"id", id}, {"method", method}, {"params", params}}, &error))
        return {};
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    loop.exec();
    QObject::disconnect(connection);
    return response;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    quizpane::ProviderLoader loader;
    QString error;
    if (!loader.load(QString::fromUtf8(DEMO_PROVIDER_PATH), &error)) return 1;
    const auto catalog = call(loader, "catalog", "catalog.list")
                             .value("result").toObject().value("nodes").toArray();
    if (catalog.isEmpty()) return 2;
    const auto attempt = call(loader, "create", "attempt.create",
        {{"categoryId", "demo-general"}, {"count", 5}})
        .value("result").toObject();
    const QString attemptId = attempt.value("attemptId").toString();
    if (attemptId.isEmpty()) return 3;
    const auto questions = call(loader, "questions", "attempt.questions",
        {{"attemptId", attemptId}}).value("result").toObject()
        .value("questions").toArray();
    if (questions.size() != 5) return 4;
    if (call(loader, "save", "attempt.saveAnswers",
             {{"attemptId", attemptId}, {"answers", QJsonArray{}}})
            .contains("error")) return 5;
    if (call(loader, "submit", "attempt.submit", {{"attemptId", attemptId}})
            .contains("error")) return 6;
    const auto solutions = call(loader, "solutions", "attempt.solutions",
        {{"attemptId", attemptId}}).value("result").toObject()
        .value("solutions").toArray();
    return solutions.size() == 5 ? 0 : 7;
}
