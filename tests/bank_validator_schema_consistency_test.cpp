// 本测试的目的：core/src/bank_validator.cpp 里手写的字段/枚举白名单，和
// schemas/declarative-provider.schema.json 里对应定义的 "properties"/"enum"
// 必须逐一一致。两份规则由不同语言（C++ 校验器 vs. JSON Schema 文档）独立维护，
// 没有共享的代码生成，历史上已经出现过 asset 的 sourceDocument/sourcePage/
// autoCrop/crop 四个字段只在校验器里放行、schema 却因 additionalProperties:false
// 直接拒绝的漂移（题库制作器产出的题库包会被 schema 判定为不合法，但校验器认为
// 合法）。此测试把这类漂移变成 ctest 失败，而不是留到有人拿 schema 做外部校验
// 时才发现。
//
// 做法：从 schema JSON 里用固定路径把每个对象的 "properties" 键集合、每个
// 枚举的 "enum" 值集合抠出来，与 quizpane::bankSchemaKeySets() 暴露的
// 集合逐一比较。任何一边加字段却忘了改另一边，这里都会失败并打印具体差异。

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include "quizpane/bank_validator.hpp"

namespace {

QJsonObject readSchema(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

// 从 QJsonObject 的 "properties" 字段读出 schema 声明的键集合。
QSet<QString> propertyKeys(const QJsonObject& objectSchema) {
    QSet<QString> keys;
    const QJsonObject properties = objectSchema.value("properties").toObject();
    for (auto it = properties.constBegin(); it != properties.constEnd(); ++it)
        keys.insert(it.key());
    return keys;
}

// 从 QJsonObject 的 "enum" 数组读出取值集合（数组元素只处理字符串枚举，
// 本 schema 里所有用 bankSchemaKeySets().enumValues 追踪的枚举都是字符串）。
QSet<QString> enumValues(const QJsonObject& objectSchema) {
    QSet<QString> values;
    for (const auto& value : objectSchema.value("enum").toArray())
        values.insert(value.toString());
    return values;
}

// 打印两个字符串集合的差异，方便失败时一眼看出该往哪边加/删字段。
QString describeDiff(const QString& label, const QSet<QString>& fromValidator,
                     const QSet<QString>& fromSchema) {
    const QSet<QString> onlyInValidator = fromValidator - fromSchema;
    const QSet<QString> onlyInSchema = fromSchema - fromValidator;
    QString message;
    QTextStream stream(&message);
    stream << label << " 不一致：";
    if (!onlyInValidator.isEmpty())
        stream << " 校验器有但 schema 没有 = " << QStringList(onlyInValidator.values()).join(", ");
    if (!onlyInSchema.isEmpty())
        stream << " schema 有但校验器没有 = " << QStringList(onlyInSchema.values()).join(", ");
    return message;
}

bool checkKeys(const QString& label, const QSet<QString>& fromValidator,
              const QSet<QString>& fromSchema) {
    if (fromValidator == fromSchema) return true;
    qWarning("%s", qUtf8Printable(describeDiff(label, fromValidator, fromSchema)));
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QJsonObject schema = readSchema(QString::fromUtf8(DECLARATIVE_PROVIDER_SCHEMA_PATH));
    if (schema.isEmpty()) return 1;

    const auto& keySets = quizpane::bankSchemaKeySets();
    const QJsonObject defs = schema.value("$defs").toObject();
    const QJsonObject catalogItemSchema = schema.value("properties").toObject()
        .value("catalogs").toObject().value("items").toObject();
    const QJsonObject questionSchema = defs.value("question").toObject();
    const QJsonObject materialSchema = defs.value("material").toObject();

    // objectFields：每个 key 对应 schema 里的一处 "properties" 定义。
    struct ObjectCheck { QString label; QSet<QString> validatorKeys; QJsonObject schemaObject; };
    const QList<ObjectCheck> objectChecks{
        {"bank", keySets.objectFields.value("bank"), schema},
        {"catalog", keySets.objectFields.value("catalog"), catalogItemSchema},
        {"practice", keySets.objectFields.value("practice"),
            catalogItemSchema.value("properties").toObject().value("practice").toObject()},
        {"material", keySets.objectFields.value("material"), materialSchema},
        {"materialSource", keySets.objectFields.value("materialSource"), defs.value("materialSource").toObject()},
        {"source", keySets.objectFields.value("source"), defs.value("source").toObject()},
        {"asset", keySets.objectFields.value("asset"), defs.value("asset").toObject()},
        {"normalizedCrop", keySets.objectFields.value("normalizedCrop"), defs.value("normalizedCrop").toObject()},
        {"underline", keySets.objectFields.value("underline"),
            materialSchema.value("properties").toObject().value("underlines").toObject()
                .value("items").toObject()},
        // material.review 与 question.review 是 schema 里两处内联但结构相同的定义，
        // 校验器把它们合并成一份 reviewKeys()；这里只需要对其中一处做比对。
        {"review", keySets.objectFields.value("review"),
            questionSchema.value("properties").toObject().value("review").toObject()},
        {"option", keySets.objectFields.value("option"), defs.value("option").toObject()},
        {"question", keySets.objectFields.value("question"), questionSchema},
        {"answer", keySets.objectFields.value("answer"),
            questionSchema.value("properties").toObject().value("answer").toObject()},
    };

    bool allMatched = true;
    for (const auto& check : objectChecks) {
        if (!checkKeys(check.label, check.validatorKeys, propertyKeys(check.schemaObject)))
            allMatched = false;
    }

    // enumValues：每个 key 对应 schema 里一处 "enum" 定义。
    struct EnumCheck { QString label; QSet<QString> validatorValues; QJsonObject schemaObject; };
    const QList<EnumCheck> enumChecks{
        {"practiceMode", keySets.enumValues.value("practiceMode"),
            catalogItemSchema.value("properties").toObject().value("practice").toObject()
                .value("properties").toObject().value("mode").toObject()},
        {"questionType", keySets.enumValues.value("questionType"),
            questionSchema.value("properties").toObject().value("type").toObject()},
        {"riskLevel", keySets.enumValues.value("riskLevel"),
            questionSchema.value("properties").toObject().value("review").toObject()
                .value("properties").toObject().value("riskLevel").toObject()},
        {"answerPolicy", keySets.enumValues.value("answerPolicy"),
            schema.value("properties").toObject().value("answerPolicy").toObject()},
    };
    for (const auto& check : enumChecks) {
        if (!checkKeys(check.label, check.validatorValues, enumValues(check.schemaObject)))
            allMatched = false;
    }

    return allMatched ? 0 : 100;
}
