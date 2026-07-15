#pragma once

#include <QJsonArray>
#include <QHash>
#include <QByteArray>
#include <QStringList>

namespace quizpane::studio {

// 规则与模型生成共用的唯一中间结果。两条路径都必须把可直接采用与待人工
// 复核的题目分开，避免 workflow 再维护一套不同的降级语义。
struct ReviewResult {
    QJsonArray materials;
    QJsonArray questions;
    QJsonArray needsReviewQuestions;
    QStringList warnings;
    // assets/<...>.png -> PNG bytes。该字段是制作过程中的附件清单，不写入 bank.json。
    QHash<QString, QByteArray> assets;
};

}  // namespace quizpane::studio
