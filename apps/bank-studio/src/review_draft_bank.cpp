#include "review_draft_bank.hpp"

namespace quizpane::studio {

QJsonObject makeReviewDraftBank(const QJsonObject& question, const QJsonArray& materials,
                                const bool hasAnswerKey) {
    QJsonObject bank{{QStringLiteral("schemaVersion"), 3},
                     {QStringLiteral("title"), QStringLiteral("复核草稿")},
                     {QStringLiteral("answerPolicy"), hasAnswerKey
                         ? QStringLiteral("included") : QStringLiteral("none")},
                     {QStringLiteral("catalogs"), QJsonArray{QJsonObject{
                         {QStringLiteral("id"), QStringLiteral("generated")},
                         {QStringLiteral("title"), QStringLiteral("复核草稿")},
                         {QStringLiteral("practice"), QJsonObject{
                             {QStringLiteral("mode"), QStringLiteral("all")}}}}}},
                     {QStringLiteral("questions"), QJsonArray{question}}};
    if (!materials.isEmpty())
        bank.insert(QStringLiteral("materials"), materials);
    return bank;
}

}  // namespace quizpane::studio
