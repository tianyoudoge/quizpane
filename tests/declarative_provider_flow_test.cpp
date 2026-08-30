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
    const QJsonArray answerlessBankQuestions{
        QJsonObject{{"id", "q-set1-1"}, {"catalogId", "generated"},
            {"type", "single_choice"}, {"stem", QStringLiteral("甲乙〔填空〕丙")},
            {"stemUnderlines", QJsonArray{QJsonObject{{"start", 0}, {"length", 1}}}},
            {"options", QJsonArray{QJsonObject{{"id", "a"}, {"text", "甲"}},
                                    QJsonObject{{"id", "b"}, {"text", "乙"}}}},
            {"source", QJsonObject{{"document", "original.pdf"}, {"questionNumber", "1-1"},
                                    {"sectionId", "set-1"}, {"sectionTitle", QStringLiteral("第一套")}}}},
        QJsonObject{{"id", "q-set1-2"}, {"catalogId", "generated"},
            {"type", "single_choice"}, {"stem", QStringLiteral("第一套第二题")},
            {"options", QJsonArray{QJsonObject{{"id", "a"}, {"text", "甲"}},
                                    QJsonObject{{"id", "b"}, {"text", "乙"}}}},
            {"source", QJsonObject{{"document", "original.pdf"}, {"questionNumber", "1-2"},
                                    {"sectionId", "set-1"}, {"sectionTitle", QStringLiteral("第一套")}}}},
        QJsonObject{{"id", "q-set2-1"}, {"catalogId", "generated"},
            {"type", "single_choice"}, {"stem", QStringLiteral("第二套第一题")},
            {"options", QJsonArray{QJsonObject{{"id", "a"}, {"text", "甲"}},
                                    QJsonObject{{"id", "b"}, {"text", "乙"}}}},
            {"source", QJsonObject{{"document", "original.pdf"}, {"questionNumber", 1},
                                    {"questionLabel", "1"}, {"sectionId", "set-2"},
                                    {"sectionTitle", QStringLiteral("第二套")}}}}};
    const QJsonObject bank{{"schemaVersion", 3}, {"answerPolicy", "none"}, {"title", "No answer"},
        {"catalogs", QJsonArray{QJsonObject{{"id", "generated"}, {"title", "No answer"},
            {"practice", QJsonObject{{"mode", "random"}}}}}},
        {"questions", answerlessBankQuestions}};
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
    const QJsonArray answerlessNodes = call(answerlessLoader, "no-answer-catalog", "catalog.list")
        .value("result").toObject().value("nodes").toArray();
    if (answerlessNodes.size() != 2) return 17;
    QString firstSetId;
    for (const auto& value : answerlessNodes) {
        const QJsonObject node = value.toObject();
        if (node.value("practiceMode").toString() != QStringLiteral("sequential")) return 18;
        if (node.value("title").toString() == QStringLiteral("第一套"))
            firstSetId = node.value("id").toString();
    }
    if (firstSetId.isEmpty()) return 19;
    const auto answerlessAttempt = call(answerlessLoader, "no-answer-create", "attempt.create",
        {{"categoryId", firstSetId}, {"count", 2}}).value("result").toObject();
    if (answerlessAttempt.value("hasAnswerKey").toBool(true)) return 12;
    const auto answerlessQuestions = call(answerlessLoader, "no-answer-questions", "attempt.questions",
        {{"attemptId", answerlessAttempt.value("attemptId")}}).value("result").toObject().value("questions").toArray();
    if (answerlessQuestions.size() != 2 || answerlessQuestions.first().toObject().contains("correctChoice") ||
        answerlessQuestions.first().toObject().value("id").toString() != QStringLiteral("q-set1-1") ||
        answerlessQuestions.at(1).toObject().value("id").toString() != QStringLiteral("q-set1-2")) return 13;
    const auto rendered = answerlessQuestions.first().toObject();
    const auto html = rendered.value("contentHtml").toString();
    if (!html.contains(QStringLiteral("第一套")) || !html.contains(QStringLiteral("1-1")) ||
        !html.contains(QStringLiteral("text-decoration:underline")) ||
        !html.contains(QStringLiteral("&nbsp;&nbsp;")) || html.contains(QStringLiteral("〔填空〕")) ||
        rendered.value("sourceQuestionLabel").toString() != QStringLiteral("1-1") ||
        rendered.contains("sourceQuestionNumber")) return 16;
    if (call(answerlessLoader, "no-answer-save", "attempt.saveAnswers", QJsonObject{{"answers", QJsonArray{
        QJsonObject{{"questionIndex", 0}, {"answer", QJsonObject{{"choice", "0"}}}}}}}).contains("error")) return 14;
    const auto answerlessReport = call(answerlessLoader, "no-answer-report", "attempt.report")
        .value("result").toObject();
    if (answerlessReport.value("hasAnswerKey").toBool(true) ||
        answerlessReport.contains("correctCount") || answerlessReport.value("answerCount").toInt() != 1) return 15;

    // 同一份多套题改为含答案后，解析页仍须保留字符串题号和套名；不能在
    // attempt.solutions 阶段重新把 questionNumber 强转成 0。
    QJsonObject answerfulBank = bank;
    answerfulBank.insert("answerPolicy", QStringLiteral("included"));
    QJsonArray answerfulQuestions = answerfulBank.value("questions").toArray();
    for (qsizetype index = 0; index < answerfulQuestions.size(); ++index) {
        QJsonObject question = answerfulQuestions.at(index).toObject();
        question.insert("answer", QJsonObject{{"optionIds", QJsonArray{"a"}}});
        question.insert("solution", QStringLiteral("参考解析"));
        answerfulQuestions.replace(index, question);
    }
    answerfulBank.insert("questions", answerfulQuestions);
    QFile answerfulFile(bankFile.fileName());
    if (!answerfulFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 20;
    answerfulFile.write(QJsonDocument(answerfulBank).toJson());
    answerfulFile.close();
    quizpane::ProviderLoader answerfulLoader;
    if (!answerfulLoader.load(bankFile.fileName(), &error)) return 21;
    const QJsonArray answerfulNodes = call(answerfulLoader, "answerful-catalog", "catalog.list")
        .value("result").toObject().value("nodes").toArray();
    QString answerfulFirstSetId;
    for (const auto& value : answerfulNodes)
        if (value.toObject().value("title").toString() == QStringLiteral("第一套"))
            answerfulFirstSetId = value.toObject().value("id").toString();
    if (answerfulFirstSetId.isEmpty()) return 22;
    call(answerfulLoader, "answerful-create", "attempt.create",
         {{"categoryId", answerfulFirstSetId}, {"count", 2}});
    const QJsonArray answerfulSolutions = call(answerfulLoader, "answerful-solutions", "attempt.solutions")
        .value("result").toObject().value("solutions").toArray();
    bool foundStringNumber = false;
    for (const auto& value : answerfulSolutions) {
        const QJsonObject solution = value.toObject();
        if (solution.value("id").toString() != QStringLiteral("q-set1-1")) continue;
        foundStringNumber = solution.value("sourceQuestionLabel").toString() == QStringLiteral("1-1") &&
            solution.value("contentHtml").toString().contains(QStringLiteral("第一套")) &&
            solution.value("contentHtml").toString().contains(QStringLiteral("1-1")) &&
            !solution.contains("sourceQuestionNumber") && solution.contains("correctChoices");
    }
    if (!foundStringNumber) return 23;
    return 0;
}
