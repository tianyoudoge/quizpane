#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QSet>
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
// schemas/declarative-provider.schema.json（兼容 schemaVersion=2/3）。顶层可选的 materials 数组和
// 题目 materialId 用于表达共享材料/题组（同一篇文章、图表贯穿多道子题）；
// 不含 materials 的题库按普通独立题校验。tools/bank-generator 和题库制作器
// 共用这一份规则，避免两处校验标准逐渐漂移。
QList<BankValidationError> validateBankDetailed(const QJsonObject& bank);

// 兼容旧调用方式：只关心是否通过和第一条错误信息。
bool validateBank(const QJsonObject& bank, QString* error);

// 本文件校验规则与 schemas/declarative-provider.schema.json 是两份手写的独立
// 实现（C++ 校验器需要在没有 JSON Schema 库依赖的前提下跑在 Win7 x86 上），
// 天然存在"改了一处忘了改另一处"的漂移风险。BankSchemaKeySets 把校验器内部
// 每个对象/枚举允许的字段或取值都收集起来，供 tests/bank_validator_schema_consistency_test.cpp
// 逐一比对 schema 文件里同名定义的 "properties"/"enum"，把漂移变成能被
// ctest 捕获的失败，而不是留到运行时才被用户的题库包炸出来。
//
// objectFields 的 key 是本结构体和 schema 里 $defs 名字的约定映射（bank 对应
// schema 顶层 properties，其余对应 $defs.<key>），value 是该对象允许出现的
// 字段名集合。enumValues 同理，对应 schema 里用 "enum" 声明的取值集合。
struct BankSchemaKeySets {
    QHash<QString, QSet<QString>> objectFields;
    QHash<QString, QSet<QString>> enumValues;
};

const BankSchemaKeySets& bankSchemaKeySets();

}  // namespace quizpane
