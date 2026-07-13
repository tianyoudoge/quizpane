#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "quizpane/bank_validator.hpp"

namespace {

QJsonObject readBank(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

QJsonObject mutateFirstQuestion(QJsonObject bank, const char* key, const QJsonValue& value) {
    QJsonArray questions = bank.value("questions").toArray();
    QJsonObject question = questions.first().toObject();
    question.insert(key, value);
    questions.replace(0, question);
    bank.insert("questions", questions);
    return bank;
}

// 按 id 定位共享材料 fixture 里的某道题，返回其在 questions 数组中的下标。
qsizetype questionIndexById(const QJsonArray& questions, const QString& id) {
    for (qsizetype i = 0; i < questions.size(); ++i)
        if (questions.at(i).toObject().value("id").toString() == id) return i;
    return -1;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QJsonObject validBank = readBank(QString::fromUtf8(DECLARATIVE_BANK_PATH));
    if (validBank.isEmpty()) return 1;

    // 合法题库必须通过，且没有任何错误。
    if (!quizpane::validateBankDetailed(validBank).isEmpty()) return 2;
    QString error;
    if (!quizpane::validateBank(validBank, &error) || !error.isEmpty()) return 3;

    // 重复题目 id：复制第一题但不改 id，应报告为无效。
    {
        QJsonObject bank = validBank;
        QJsonArray questions = bank.value("questions").toArray();
        questions.append(questions.first());
        bank.insert("questions", questions);
        const auto errors = quizpane::validateBankDetailed(bank);
        if (errors.isEmpty()) return 4;
    }

    // 答案引用不存在的选项。
    {
        const QJsonObject bank = mutateFirstQuestion(validBank, "answer",
            QJsonObject{{"optionIds", QJsonArray{"not-a-real-option"}}});
        const auto errors = quizpane::validateBankDetailed(bank);
        if (errors.isEmpty()) return 5;
        if (errors.first().questionIndex != 0) return 6;
    }

    // catalog 缺失：题目引用了不存在的 catalogId。
    {
        const QJsonObject bank = mutateFirstQuestion(validBank, "catalogId",
            QJsonValue(QStringLiteral("missing-catalog")));
        const auto errors = quizpane::validateBankDetailed(bank);
        if (errors.isEmpty()) return 7;
    }

    // 空白题干。
    {
        const QJsonObject bank = mutateFirstQuestion(validBank, "stem", QJsonValue(QString()));
        const auto errors = quizpane::validateBankDetailed(bank);
        if (errors.isEmpty()) return 8;
    }

    // solution 是 Schema 必填字段，即使允许空字符串也不能缺失。
    {
        QJsonObject bank = validBank;
        QJsonArray questions = bank.value("questions").toArray();
        QJsonObject question = questions.first().toObject();
        question.remove("solution");
        questions.replace(0, question);
        bank.insert("questions", questions);
        if (quizpane::validateBankDetailed(bank).isEmpty()) return 9;
    }

    // ===== 共享材料 =====
    const QJsonObject validBankWithMaterials = readBank(QString::fromUtf8(DECLARATIVE_MATERIALS_BANK_PATH));
    if (validBankWithMaterials.isEmpty()) return 10;

    // 合法的“2 份材料 + 多道子题 + 独立题”必须通过。
    if (!quizpane::validateBankDetailed(validBankWithMaterials).isEmpty()) return 11;

    // 重复材料 ID。
    {
        QJsonObject bank = validBankWithMaterials;
        QJsonArray materials = bank.value("materials").toArray();
        QJsonObject duplicate = materials.first().toObject();
        materials.append(duplicate);
        bank.insert("materials", materials);
        const auto errors = quizpane::validateBankDetailed(bank);
        if (errors.isEmpty()) return 12;
    }

    // 题目引用不存在的材料。
    {
        QJsonObject bank = validBankWithMaterials;
        QJsonArray questions = bank.value("questions").toArray();
        const qsizetype index = questionIndexById(questions, QStringLiteral("q-material1-1"));
        if (index < 0) return 13;
        QJsonObject question = questions.at(index).toObject();
        question.insert("materialId", QStringLiteral("material-missing"));
        questions.replace(index, question);
        bank.insert("questions", questions);
        const auto errors = quizpane::validateBankDetailed(bank);
        if (errors.isEmpty()) return 14;
        bool foundMaterialError = false;
        for (const auto& error : errors)
            if (error.materialId == QStringLiteral("material-missing")) foundMaterialError = true;
        if (!foundMaterialError) return 15;
    }

    // 材料和题目分类不一致。
    {
        QJsonObject bank = validBankWithMaterials;
        QJsonArray catalogs = bank.value("catalogs").toArray();
        catalogs.append(QJsonObject{{"id", "other"}, {"title", "其他分类"},
            {"practice", QJsonObject{{"mode", "sequential"}, {"questionCount", 5}}}});
        bank.insert("catalogs", catalogs);
        QJsonArray materials = bank.value("materials").toArray();
        QJsonObject material = materials.first().toObject();
        material.insert("catalogId", QStringLiteral("other"));
        materials.replace(0, material);
        bank.insert("materials", materials);
        const auto errors = quizpane::validateBankDetailed(bank);
        if (errors.isEmpty()) return 16;
    }

    // 空材料：既没有 body 也没有 images。
    {
        QJsonObject bank = validBankWithMaterials;
        QJsonArray materials = bank.value("materials").toArray();
        QJsonObject material = materials.first().toObject();
        material.remove("body");
        materials.replace(0, material);
        bank.insert("materials", materials);
        const auto errors = quizpane::validateBankDetailed(bank);
        if (errors.isEmpty()) return 17;
    }

    // 孤立材料：没有任何题目引用。
    {
        QJsonObject bank = validBankWithMaterials;
        QJsonArray materials = bank.value("materials").toArray();
        materials.append(QJsonObject{{"id", "material-orphan"}, {"catalogId", "verbal"},
            {"body", "这是一段孤立材料，没有任何题目引用它，长度足够触发校验。"}});
        bank.insert("materials", materials);
        const auto errors = quizpane::validateBankDetailed(bank);
        if (errors.isEmpty()) return 18;
        bool foundOrphanError = false;
        for (const auto& error : errors)
            if (error.materialId == QStringLiteral("material-orphan")) foundOrphanError = true;
        if (!foundOrphanError) return 19;
    }

    // 材料正文被完整复制进子题 stem。
    {
        QJsonObject bank = validBankWithMaterials;
        QJsonArray materials = bank.value("materials").toArray();
        const QString body = materials.first().toObject().value("body").toString();
        QJsonArray questions = bank.value("questions").toArray();
        const qsizetype index = questionIndexById(questions, QStringLiteral("q-material1-1"));
        if (index < 0) return 20;
        QJsonObject question = questions.at(index).toObject();
        question.insert("stem", body + QStringLiteral("\n根据材料，下列理解正确的是？"));
        questions.replace(index, question);
        bank.insert("questions", questions);
        const auto errors = quizpane::validateBankDetailed(bank);
        if (errors.isEmpty()) return 21;
    }

    // 独立题库回归通过（重复用最初读取的 validBank）。
    if (!quizpane::validateBankDetailed(validBank).isEmpty()) return 22;

    return 0;
}
