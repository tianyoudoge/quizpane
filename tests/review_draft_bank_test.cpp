#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>

#include "quizpane/bank_validator.hpp"
#include "review_draft_bank.hpp"

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QFile fixture(QString::fromUtf8(DECLARATIVE_BANK_PATH));
    if (!fixture.open(QIODevice::ReadOnly)) return 1;
    const QJsonObject sourceBank = QJsonDocument::fromJson(fixture.readAll()).object();
    QJsonObject question = sourceBank.value(QStringLiteral("questions")).toArray().first().toObject();
    question.insert(QStringLiteral("catalogId"), QStringLiteral("generated"));
    question.insert(QStringLiteral("source"), QJsonObject{
        {QStringLiteral("document"), QStringLiteral("source.pdf")},
        {QStringLiteral("questionNumber"), 1}});

    // 这是复核页“保存草稿”提交给正式 Schema 校验的最小题库形状。
    // 若分类漏掉 practice，用户会被“请修正后再保存”阻止。
    QString error;
    if (!quizpane::validateBank(
            quizpane::studio::makeReviewDraftBank(question, {}, true), &error))
        return 2;
    return 0;
}
