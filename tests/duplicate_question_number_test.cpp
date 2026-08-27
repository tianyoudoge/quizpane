#include "quizpane/studio/rule_based_generator.hpp"
#include "quizpane/bank_validator.hpp"
#include "quizpane/declarative_provider.hpp"
#include "quizpane/provider_installer.hpp"
#include "quizpane/zip_archive.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QSettings>
#include <QBuffer>
#include <QImage>
#include <QDebug>

using namespace quizpane::studio;
namespace {
RuleBasedGenerationResult generate(const QString& text, bool hasAnswers = true) {
    ExtractedDocument doc; doc.sourcePath = "same-number.txt"; doc.plainText = text;
    return RuleBasedBankGenerator{}.generate({doc}, hasAnswers);
}
QString answer(const QJsonValue& value) {
    QString result;
    for (const auto& id : value.toObject().value("answer").toObject().value("optionIds").toArray()) result += id.toString();
    return result;
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("QuizPane Tests"));
    QTemporaryDir settingsDirectory;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDirectory.path());
    const QString repeated = QStringLiteral("1.甲（A）\nA.甲\nB.乙\n1.乙（B）\nA.甲\nB.乙\n");
    const auto direct = generate(repeated);
    if (direct.questions.size() != 2 || !direct.needsReviewQuestions.isEmpty() ||
        answer(direct.questions[0]) != "a" || answer(direct.questions[1]) != "b") return 1;
    const auto first = direct.questions[0].toObject(), second = direct.questions[1].toObject();
    if (first.value("id") == second.value("id") ||
        first.value("source").toObject().value("questionLabel") == second.value("source").toObject().value("questionLabel")) return 2;

    // 重号时，题外的同号答案/解析均不传播；本题直接给出的答案仍可采用。
    const auto withTable = generate(repeated + QStringLiteral("答案\n1.A\n解析：这份解析无法确定归属\n"));
    if (withTable.questions.size() != 2 || !withTable.needsReviewQuestions.isEmpty()) return 3;
    for (const auto& q : withTable.questions)
        if (!q.toObject().value("solution").toString().isEmpty()) return 4;
    const QString noDirect = QStringLiteral("1.甲题\nA.甲\nB.乙\n1.乙题\nA.甲\nB.乙\n");
    const auto ambiguous = generate(noDirect + QStringLiteral("答案\n1.A\n解析：不应复制\n"));
    if (!ambiguous.questions.isEmpty() || ambiguous.needsReviewQuestions.size() != 2) return 5;
    for (const auto& q : ambiguous.needsReviewQuestions)
        if (!answer(q).isEmpty() || !q.toObject().value("solution").toString().isEmpty() ||
            !q.toObject().value("review").toObject().value("reason").toString().contains(QStringLiteral("归属"))) return 6;
    const auto mixed = generate(QStringLiteral("1.甲（A）\nA.甲\nB.乙\n1.乙题\nA.甲\nB.乙\n答案\n1.B\n"));
    if (mixed.questions.size() != 1 || mixed.needsReviewQuestions.size() != 1 ||
        answer(mixed.questions.first()) != "a" || !answer(mixed.needsReviewQuestions.first()).isEmpty()) return 7;

    const auto leading = generate(QStringLiteral("正确答案：A\n正确答案：B\n") + noDirect);
    if (leading.questions.size() != 2 || answer(leading.questions[0]) != "a" || answer(leading.questions[1]) != "b") return 8;
    const auto insufficient = generate(QStringLiteral("正确答案：A\n") + noDirect);
    if (insufficient.needsReviewQuestions.size() != 2) return 9;
    const auto conflictingTable = generate(QStringLiteral("1.甲题\nA.甲\nB.乙\n答案\n1.A\n1.B\n"));
    if (!conflictingTable.questions.isEmpty() || conflictingTable.needsReviewQuestions.size() != 1 ||
        !answer(conflictingTable.needsReviewQuestions.first()).isEmpty()) return 10;
    const auto sameTable = generate(QStringLiteral("1.甲题\nA.甲\nB.乙\n答案\n1.A\n1.A\n"));
    if (sameTable.questions.size() != 1 || answer(sameTable.questions.first()) != "a") return 11;
    const auto descending = generate(QStringLiteral("2.乙题\nA.甲\nB.乙\n1.甲题\nA.甲\nB.乙\n答案\n1.A\n2.B\n"));
    if (descending.questions.size() != 2 || answer(descending.questions[0]) != "b" || answer(descending.questions[1]) != "a") return 12;
    const auto answerless = generate(noDirect, false);
    if (answerless.questions.size() != 2 || !answerless.needsReviewQuestions.isEmpty()) return 13;

    ExtractedDocument visual; visual.sourcePath = "visual.txt"; visual.hasPageBoundaries = true;
    visual.plainText = QStringLiteral("1.如图所示（A）\nA.甲\nB.乙\n1.如图所示（B）\nA.甲\nB.乙\n");
    visual.questionAnchors[1] = {{"1", QRectF(0.1, 0.1, 0.02, 0.02)}, {"1", QRectF(0.1, 0.5, 0.02, 0.02)}};
    QImage page(400, 600, QImage::Format_RGB32); page.fill(Qt::white);
    QByteArray png; QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly); page.save(&buffer, "PNG");
    visual.pageImages.insert(1, png);
    const auto visualResult = RuleBasedBankGenerator{}.generate({visual});
    if (!visualResult.questions.isEmpty() || visualResult.needsReviewQuestions.size() != 2) return 28;
    for (const auto& value : visualResult.needsReviewQuestions)
        if (!value.toObject().value("review").toObject().value("reason").toString().contains(QStringLiteral("无法唯一定位题图"))) return 29;

    QJsonObject bank{{"schemaVersion", 3}, {"answerPolicy", "included"}, {"title", "Repeated numbers"},
        {"catalogs", QJsonArray{QJsonObject{{"id", "generated"}, {"title", "Repeated numbers"},
            {"practice", QJsonObject{{"mode", "all"}}}}}}, {"questions", direct.questions}};
    QString error;
    if (!quizpane::validateBank(bank, &error)) { qCritical() << error; return 14; }
    QTemporaryDir directory;
    const QJsonObject manifest{{"manifestVersion", 2}, {"id", "local.duplicate-test"}, {"name", "Test"},
        {"version", "1.0.0"}, {"kind", "declarative"}, {"permissions", QJsonObject{{"network", false}}},
        {"runtime", QJsonObject{{"format", "quizpane.bank+json"}, {"schemaVersion", 3}, {"entry", "content/bank.json"}}}};
    QFile manifestFile(directory.filePath("manifest.json"));
    if (!manifestFile.open(QIODevice::WriteOnly)) return 15;
    manifestFile.write(QJsonDocument(manifest).toJson()); manifestFile.close();
    QDir().mkpath(directory.filePath("content"));
    QFile file(directory.filePath("content/bank.json"));
    if (!file.open(QIODevice::WriteOnly)) return 15;
    file.write(QJsonDocument(bank).toJson()); file.close();
    quizpane::DeclarativeProvider provider;
    if (!provider.load(file.fileName(), &error)) { qCritical() << error; return 16; }
    auto result = provider.request({{"id", "create"}, {"method", "attempt.create"},
        {"params", QJsonObject{{"categoryId", "generated"}, {"count", 2}}}});
    if (result.contains("error")) return 17;
    const auto hosted = provider.request({{"id", "questions"}, {"method", "attempt.questions"}})
        .value("result").toObject().value("questions").toArray();
    if (hosted.size() != 2 || hosted[0].toObject().value("id") == hosted[1].toObject().value("id") ||
        hosted[0].toObject().value("sourceQuestionNumber") != hosted[1].toObject().value("sourceQuestionNumber") ||
        hosted[0].toObject().value("sourceQuestionLabel") == hosted[1].toObject().value("sourceQuestionLabel")) return 18;
    for (const auto& value : hosted)
        if (!value.toObject().value("contentHtml").toString().contains(QStringLiteral("同号第"))) return 19;
    result = provider.request({{"id", "save"}, {"method", "attempt.saveAnswers"},
        {"params", QJsonObject{{"answers", QJsonArray{
            QJsonObject{{"questionIndex", 0}, {"answer", QJsonObject{{"choice", "0"}}}},
            QJsonObject{{"questionIndex", 1}, {"answer", QJsonObject{{"choice", "1"}}}}}}}}});
    if (result.contains("error")) return 20;
    const auto report = provider.request({{"id", "report"}, {"method", "attempt.report"}}).value("result").toObject();
    if (report.value("correctCount").toInt() != 2) return 21;
    // 修改一题的答案不能覆盖另一道同号题。
    result = provider.request({{"id", "save-again"}, {"method", "attempt.saveAnswers"},
        {"params", QJsonObject{{"answers", QJsonArray{
            QJsonObject{{"questionIndex", 1}, {"answer", QJsonObject{{"choice", "0"}}}}}}}}});
    if (result.contains("error")) return 20;
    if (provider.request({{"id", "report-again"}, {"method", "attempt.report"}}).value("result").toObject()
        .value("correctCount").toInt() != 1) return 22;
    result = provider.request({{"id", "history"}, {"method", "attempt.create"},
        {"params", QJsonObject{{"categoryId", "generated"}, {"count", 2}}}});
    const auto remaining = provider.request({{"id", "remaining"}, {"method", "attempt.questions"}})
        .value("result").toObject().value("questions").toArray();
    if (result.contains("error") || remaining.size() != 1 || remaining.first().toObject().value("id") != second.value("id")) return 27;

    const QString package = directory.filePath("repeated.quizpane-provider");
    if (!quizpane::writeZipArchive(package, {{"manifest.json", QJsonDocument(manifest).toJson()},
        {"content/bank.json", QJsonDocument(bank).toJson()}}, &error)) return 23;
    quizpane::ProviderPackageInfo info;
    if (!quizpane::ProviderInstaller{}.inspect(package, &info, &error)) { qCritical() << error; return 24; }
    bank.insert("questions", descending.questions);
    if (!quizpane::validateBank(bank, &error)) return 25;
    bank.insert("questions", answerless.questions); bank.insert("answerPolicy", "none");
    if (!quizpane::validateBank(bank, &error)) return 26;
    return 0;
}
