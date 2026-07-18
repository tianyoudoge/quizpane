#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
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
    if (solutions.size() != 1 || solutions.first().toObject().value("correctChoice").toInt() != 2)
        return 8;

    // 无答案题库不会返回评分或解析，但作答仍可保存、统计并导出到 Host。
    QTemporaryDir directory;
    if (!directory.isValid()) return 9;
    const QJsonObject bank{{"schemaVersion", 3}, {"answerPolicy", "none"}, {"title", "No answer"},
        {"catalogs", QJsonArray{QJsonObject{{"id", "generated"}, {"title", "No answer"},
            {"practice", QJsonObject{{"mode", "all"}}}}}},
        {"questions", QJsonArray{QJsonObject{{"id", "q76"}, {"catalogId", "generated"},
            {"type", "single_choice"}, {"stem", "第 76 题"},
            {"options", QJsonArray{QJsonObject{{"id", "a"}, {"text", "甲"}},
                                    QJsonObject{{"id", "b"}, {"text", "乙"}}}},
            {"source", QJsonObject{{"document", "original.pdf"}, {"questionNumber", 76},
                                    {"questionLabel", "76"}}}}}}};
    const QJsonObject manifest{{"manifestVersion", 2}, {"id", "org.quizpane.no-answer-test"},
        {"name", "No answer"}, {"version", "1.0.0"}, {"kind", "declarative"},
        {"runtime", QJsonObject{{"format", "quizpane.bank+json"}, {"schemaVersion", 3},
            {"entry", "bank.json"}}}, {"permissions", QJsonObject{{"network", false}}}};
    QFile manifestFile(directory.filePath("manifest.json"));
    QFile bankFile(directory.filePath("bank.json"));
    if (!manifestFile.open(QIODevice::WriteOnly) || !bankFile.open(QIODevice::WriteOnly)) return 10;
    manifestFile.write(QJsonDocument(manifest).toJson());
    bankFile.write(QJsonDocument(bank).toJson());
    manifestFile.close();
    bankFile.close();
    quizpane::ProviderLoader answerlessLoader;
    if (!answerlessLoader.load(bankFile.fileName(), &error)) return 11;
    const auto answerlessAttempt = call(answerlessLoader, "no-answer-create", "attempt.create",
        {{"categoryId", "generated"}, {"count", 1}}).value("result").toObject();
    if (answerlessAttempt.value("hasAnswerKey").toBool(true)) return 12;
    const auto answerlessQuestions = call(answerlessLoader, "no-answer-questions", "attempt.questions",
        {{"attemptId", answerlessAttempt.value("attemptId")}}).value("result").toObject().value("questions").toArray();
    if (answerlessQuestions.size() != 1 || answerlessQuestions.first().toObject().contains("correctChoice")) return 13;
    if (call(answerlessLoader, "no-answer-save", "attempt.saveAnswers", QJsonObject{{"answers", QJsonArray{
        QJsonObject{{"questionIndex", 0}, {"answer", QJsonObject{{"choice", "0"}}}}}}}).contains("error")) return 14;
    const auto answerlessReport = call(answerlessLoader, "no-answer-report", "attempt.report")
        .value("result").toObject();
    if (answerlessReport.value("hasAnswerKey").toBool(true) ||
        answerlessReport.contains("correctCount") || answerlessReport.value("answerCount").toInt() != 1) return 15;
    return 0;
}
