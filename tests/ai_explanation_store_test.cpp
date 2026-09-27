#include "../apps/desktop-qt/src/ai/ai_explanation_store.hpp"
#include "quizpane/bank_validator.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    if (!directory.isValid()) return 1;
    const QString path = directory.filePath(QStringLiteral("bank.json"));
    if (!QFile::copy(QString::fromUtf8(DECLARATIVE_BANK_PATH), path)) return 2;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return 3;
    QJsonObject bank = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    const QString id = bank.value(QStringLiteral("questions")).toArray().first()
                           .toObject().value(QStringLiteral("id")).toString();
    const QString html = QStringLiteral("<div>解题步骤：x<sup>2</sup> + √4 = 6</div>");
    QString error;
    if (!quizpane::saveAiExplanation(path, id, html, &error)) return 4;
    if (quizpane::loadAiExplanations(path).value(id) != html) return 5;
    if (!file.open(QIODevice::ReadOnly)) return 6;
    bank = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    if (!quizpane::validateBankDetailed(bank).isEmpty() ||
        bank.value(QStringLiteral("questions")).toArray().first()
            .toObject().value(QStringLiteral("aiSolution")).toString() != html) return 7;
    if (quizpane::saveAiExplanation(path, QStringLiteral("missing"), html, &error)) return 8;

    // Answerless banks retain their no-answer policy while persisting AI text.
    bank.insert(QStringLiteral("schemaVersion"), 3);
    bank.insert(QStringLiteral("answerPolicy"), QStringLiteral("none"));
    QJsonArray questions = bank.value(QStringLiteral("questions")).toArray();
    for (qsizetype i = 0; i < questions.size(); ++i) {
        QJsonObject question = questions.at(i).toObject();
        question.remove(QStringLiteral("answer"));
        question.remove(QStringLiteral("solution"));
        question.insert(QStringLiteral("source"), QJsonObject{
            {QStringLiteral("document"), QStringLiteral("test.pdf")},
            {QStringLiteral("questionNumber"), int(i + 1)}});
        questions.replace(i, question);
    }
    bank.insert(QStringLiteral("questions"), questions);
    if (!quizpane::validateBankDetailed(bank).isEmpty()) return 9;
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 10;
    file.write(QJsonDocument(bank).toJson());
    file.close();
    if (!quizpane::saveAiExplanation(path, id, html + QStringLiteral(" 再次生成"), &error)) return 11;
    if (!file.open(QIODevice::ReadOnly)) return 12;
    bank = QJsonDocument::fromJson(file.readAll()).object();
    if (bank.value(QStringLiteral("answerPolicy")) != QStringLiteral("none") ||
        !quizpane::validateBankDetailed(bank).isEmpty()) return 13;
    return 0;
}
