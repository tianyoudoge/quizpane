#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>

#include "quizpane/provider_loader.hpp"

namespace {
QJsonObject call(quizpane::ProviderLoader& loader, const QString& id,
                 const QString& method, const QJsonObject& params = {}) {
    QEventLoop loop;
    QJsonObject response;
    const auto connection = QObject::connect(
        &loader, &quizpane::ProviderLoader::responseReceived, &loop,
        [&](const QJsonObject& candidate) {
            if (candidate.value("id").toString() == id) { response = candidate; loop.quit(); }
        });
    QString error;
    if (!loader.request({{"id", id}, {"method", method}, {"params", params}}, &error)) return {};
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    loop.exec();
    QObject::disconnect(connection);
    return response;
}
}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    quizpane::ProviderLoader loader;
    QString error;
    if (!loader.load(QString::fromUtf8(DECLARATIVE_BANK_PATH), &error)) return 1;
    if (loader.descriptor().value("kind") != QStringLiteral("declarative")) return 2;
    const auto nodes = call(loader, "catalog", "catalog.list").value("result")
                           .toObject().value("nodes").toArray();
    if (nodes.size() != 1) return 3;
    const auto attempt = call(loader, "create", "attempt.create",
        {{"categoryId", "general-knowledge"}, {"count", 1},
         {"includePreviouslyAnswered", true}}).value("result").toObject();
    if (attempt.value("attemptId").toString().isEmpty()) return 4;
    const auto questions = call(loader, "questions", "attempt.questions")
                               .value("result").toObject().value("questions").toArray();
    if (questions.size() != 1 || questions.first().toObject().value("options").toArray().size() != 4) return 5;
    const QJsonArray answers{QJsonObject{{"questionIndex", 0},
        {"answer", QJsonObject{{"choice", "2"}}}}};
    if (call(loader, "save", "attempt.saveAnswers", {{"answers", answers}}).contains("error")) return 6;
    const auto report = call(loader, "report", "attempt.report").value("result").toObject();
    if (report.value("correctCount").toInt() != 1) return 7;
    const auto solutions = call(loader, "solutions", "attempt.solutions")
                               .value("result").toObject().value("solutions").toArray();
    return solutions.size() == 1 && solutions.first().toObject().value("correctChoice").toInt() == 2 ? 0 : 8;
}
