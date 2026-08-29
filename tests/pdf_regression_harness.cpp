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
#include <QSet>

namespace {
bool writeFile(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QStringList arguments = app.arguments();
    const bool embeddedAnswers = arguments.removeAll(QStringLiteral("--embedded-answers")) > 0;
    if (arguments.size() != 4 && arguments.size() != 3) {
        qCritical("usage: pdf_regression_harness [--embedded-answers] <questions.pdf> [answers.pdf] <output-dir>");
        return 2;
    }

    quizpane::studio::ExtractorRegistry extractors;
    auto questions = extractors.extract(arguments.at(1));
    const bool separateAnswers = arguments.size() == 4;
    const bool hasAnswers = separateAnswers || embeddedAnswers;
    const auto answers = separateAnswers ? extractors.extract(arguments.at(2))
                                    : quizpane::studio::ExtractedDocument{};
    if (!questions.error.isEmpty() || !answers.error.isEmpty()) {
        qCritical().noquote() << questions.error << answers.error;
        return 3;
    }
    if (separateAnswers) questions.plainText += QStringLiteral("\n\n答案及解析\n") + answers.plainText;
    const auto result = quizpane::studio::RuleBasedBankGenerator{}.generate({questions}, hasAnswers);
    // 真题不入库；本地显式启用时锁定这份 147 页、20 套题样本的回归结果。
    if (qEnvironmentVariableIsSet("QUIZPANE_VERIFY_BOOKLET")) {
        QSet<QString> sections, ids;
        bool valid = result.questions.size() == 598 && result.needsReviewQuestions.size() == 1 && result.materials.isEmpty();
        int checkedUnderlines = 0, checkedBlanks = 0;
        const QStringList expectedUnderlines{QStringLiteral("这种状况"), QStringLiteral("新的问题"),
            QStringLiteral("这"), QStringLiteral("这让北方游牧民族的日子比较好过")};
        const QList<int> expectedBlanks{1, 2, 1, 3};
        for (const auto& value : result.questions) {
            const auto q = value.toObject();
            const auto source = q.value("source").toObject();
            sections.insert(source.value("sectionId").toString());
            ids.insert(q.value("id").toString());
            const int page = source.value("page").toInt(), number = source.value("questionNumber").toInt();
            const QString stem = q.value("stem").toString();
            if (page == 67 && number >= 1 && number <= 4) {
                const auto ranges = q.value("stemUnderlines").toArray();
                valid &= ranges.size() == 1;
                if (ranges.size() == 1) {
                    const auto range = ranges.first().toObject();
                    valid &= stem.mid(range.value("start").toInt(), range.value("length").toInt()) == expectedUnderlines.at(number - 1);
                }
                ++checkedUnderlines;
            }
            if (page == 99 && number >= 1 && number <= 4) {
                valid &= stem.count(QStringLiteral("〔填空〕")) == expectedBlanks.at(number - 1);
                ++checkedBlanks;
            }
            if (page == 81 && number == 1) valid &= stem.startsWith(QStringLiteral("〔填空〕。"));
            for (const auto& option : q.value("options").toArray()) {
                const QString text = option.toObject().value("text").toString();
                valid &= !text.contains("CCtalk") && !text.contains(QStringLiteral("专项刷题")) &&
                         !text.contains(QStringLiteral("前行必有曙光"));
            }
        }
        valid &= sections.size() == 20 && ids.size() == 598 && checkedUnderlines == 4 && checkedBlanks == 4;
        if (result.needsReviewQuestions.size() == 1) {
            const auto orphan = result.needsReviewQuestions.first().toObject();
            valid &= orphan.value("source").toObject().value("sectionId").toString() == "set-8" &&
                     orphan.value("options").toArray().size() == 8 && orphan.contains("stemImage");
        }
        if (!valid) {
            writeFile(QDir(arguments.last()).filePath("failed-booklet-regression.json"),
                QJsonDocument(QJsonObject{{"questions", result.questions}, {"needsReview", result.needsReviewQuestions}}).toJson());
            qCritical("Booklet real-fixture regression failed"); return 8;
        }
    }
    const QJsonObject bank{
        {"schemaVersion", 3},
        {"answerPolicy", hasAnswers ? "included" : "none"},
        {"title", "PDF regression"},
        {"catalogs", QJsonArray{QJsonObject{{"id", "generated"}, {"title", "PDF regression"},
            {"practice", QJsonObject{{"mode", "all"}}}}}},
        {"materials", result.materials},
        {"questions", result.questions}};
    const QString output = arguments.last();
    // 校验失败也保留诊断草稿，真实题本可能有印刷重号；不把失败伪装成有效题库。
    if (!writeFile(QDir(output).filePath("review-report.json"), QJsonDocument(QJsonObject{
            {"needsReview", result.needsReviewQuestions}, {"warnings", QJsonArray::fromStringList(result.warnings)}})
            .toJson(QJsonDocument::Indented))) return 5;
    QString validationError;
    if (!quizpane::validateBank(bank, &validationError)) {
        if (!writeFile(QDir(output).filePath("invalid-candidate.json"), QJsonDocument(bank).toJson())) return 5;
        qCritical().noquote() << validationError;
        return 4;
    }
    const QJsonObject manifest{{"manifestVersion", 2}, {"id", "local.pdf-regression"},
        {"name", "PDF regression"}, {"version", "1.0.0"}, {"kind", "declarative"},
        {"runtime", QJsonObject{{"format", "quizpane.bank+json"}, {"schemaVersion", 3},
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
    qInfo().noquote() << QStringLiteral(
        "generated %1 questions, %2 materials, %3 packaged assets, %4 review previews")
        .arg(result.questions.size()).arg(result.materials.size()).arg(result.assets.size())
        .arg(result.reviewSourceImages.size());
    return 0;
}
