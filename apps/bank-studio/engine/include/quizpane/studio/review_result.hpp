#pragma once

#include <QJsonArray>
#include <QJsonObject>
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
    // 一个题库只能处于“含答案”或“无答案”之一；不能混合，避免练习结果语义不清。
    bool hasAnswerKey = true;
    // 题目 id -> 原卷裁图描述。仅供制作器校对，不写入 bank.json。
    QHash<QString, QJsonObject> reviewSourceImages;
    // 原卷校对裁图与正式题库附件分开保存，避免“每题一张预览图”撑大安装包。
    QHash<QString, QByteArray> reviewAssets;
};

}  // namespace quizpane::studio
