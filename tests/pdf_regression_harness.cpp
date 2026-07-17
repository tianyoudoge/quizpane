#include "quizpane/bank_validator.hpp"
#include "quizpane/declarative_provider.hpp"
#include "quizpane/studio/document_extractor.hpp"
#include "quizpane/studio/rule_based_generator.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace {
bool writeFile(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 4) {
        qCritical("usage: pdf_regression_harness <questions.pdf> <answers.pdf> <output-dir>");
        return 2;
    }

    quizpane::studio::ExtractorRegistry extractors;
    auto questions = extractors.extract(app.arguments().at(1));
    const auto answers = extractors.extract(app.arguments().at(2));
    if (!questions.error.isEmpty() || !answers.error.isEmpty()) {
        qCritical().noquote() << questions.error << answers.error;
        return 3;
    }
    questions.plainText += QStringLiteral("\n\n答案及解析\n") + answers.plainText;
    const auto result = quizpane::studio::RuleBasedBankGenerator{}.generate({questions});
    const QJsonObject bank{
        {"schemaVersion", 2},
        {"title", "PDF regression"},
        {"catalogs", QJsonArray{QJsonObject{{"id", "generated"}, {"title", "PDF regression"},
            {"practice", QJsonObject{{"mode", "all"}}}}}},
        {"materials", result.materials},
        {"questions", result.questions}};
    QString validationError;
    if (!quizpane::validateBank(bank, &validationError)) {
        qCritical().noquote() << validationError;
        return 4;
    }
    const QString output = app.arguments().at(3);
    const QJsonObject manifest{{"manifestVersion", 2}, {"id", "local.pdf-regression"},
        {"name", "PDF regression"}, {"version", "1.0.0"}, {"kind", "declarative"},
        {"runtime", QJsonObject{{"format", "quizpane.bank+json"}, {"schemaVersion", 2},
            {"entry", "content/bank.json"}}},
        {"permissions", QJsonObject{{"network", false}}}};
    if (!writeFile(QDir(output).filePath("manifest.json"),
                   QJsonDocument(manifest).toJson(QJsonDocument::Indented)))
        return 5;
    if (!writeFile(QDir(output).filePath("content/bank.json"),
                   QJsonDocument(bank).toJson(QJsonDocument::Indented)))
        return 5;
    for (auto it = result.assets.cbegin(); it != result.assets.cend(); ++it)
        if (!writeFile(QDir(output).filePath(it.key()), it.value()))
            return 6;
    quizpane::DeclarativeProvider provider;
    if (!provider.load(QDir(output).filePath("content/bank.json"), &validationError)) {
        qCritical().noquote() << validationError;
        return 7;
    }
    qInfo().noquote() << QStringLiteral("generated %1 questions, %2 materials, %3 assets")
        .arg(result.questions.size()).arg(result.materials.size()).arg(result.assets.size());
    return 0;
}
