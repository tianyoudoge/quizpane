#pragma once

#include <QJsonArray>
#include <QJsonObject>

namespace quizpane::studio {

// 复核页保存单题时，先用这一最小题库走完整 Schema 校验。集中构造可避免
// 草稿校验与最终打包的题库结构逐渐偏离。
QJsonObject makeReviewDraftBank(const QJsonObject& question, const QJsonArray& materials,
                                bool hasAnswerKey);

}  // namespace quizpane::studio
