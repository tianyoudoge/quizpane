#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>

namespace quizpane {

// 一条结构或语义校验错误。questionIndex 为 -1 表示错误不针对具体题目
// （例如题库标题缺失或分类重复）。materialId 非空时表示错误与某份共享材料
// 相关（材料本身的结构错误，或题目对材料的引用错误）。修复循环据此把错误
// 连同原文片段一起回传模型，只重试出错的题目或材料，而不是整份题库。
struct BankValidationError {
    int questionIndex = -1;
    QString questionId;
    QString message;
    QString materialId;
};

// 对 declarative bank.json 做结构与语义校验，规则来自
// schemas/declarative-provider.schema.json。顶层可选的 materials 数组和
// 题目 materialId 用于表达共享材料/题组（同一篇文章、图表贯穿多道子题）；
// 不含 materials 的题库按普通独立题校验。tools/bank-generator 和题库制作器
// 共用这一份规则，避免两处校验标准逐渐漂移。
QList<BankValidationError> validateBankDetailed(const QJsonObject& bank);

// 兼容旧调用方式：只关心是否通过和第一条错误信息。
bool validateBank(const QJsonObject& bank, QString* error);

}  // namespace quizpane
