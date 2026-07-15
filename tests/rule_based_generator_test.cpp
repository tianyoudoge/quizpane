#include "quizpane/bank_validator.hpp"
#include "quizpane/declarative_provider.hpp"
#include "quizpane/provider_installer.hpp"
#include "quizpane/studio/rule_based_generator.hpp"
#include "quizpane/zip_archive.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

namespace {
QJsonObject bankFor(const quizpane::studio::RuleBasedGenerationResult& result) {
    return {{"schemaVersion", 2},
            {"title", "Rules"},
            {"catalogs", QJsonArray{QJsonObject{{"id", "generated"},
                                                {"title", "Rules"},
                                                {"practice", QJsonObject{{"mode", "all"}}}}}},
            {"materials", result.materials},
            {"questions", result.questions}};
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    using namespace quizpane::studio;

    ExtractedDocument structured;
    structured.sourcePath = QStringLiteral("reading.txt");
    structured.plainText = QStringLiteral("材料一：阅读下面文字\n"
                                          "海水看起来是蓝色的，这是一段用于两道题的共享材料。\n"
                                          "根据材料回答1-2题。\n"
                                          "1. 海水看起来是什么颜色？\n"
                                          "A. 蓝色\nB. 红色\nC. 黄色\nD. 黑色\n标准答案：蓝色\n"
                                          "2. 材料包含几道子题？\n"
                                          "A. 一道\nB. 两道\nC. 三道\nD. 四道\n"
                                          "参考答案\n"
                                          "1.A\n解析：海水对光的散射使其呈现蓝色。\n"
                                          "2.B\n解析：题号范围是 1-2。\n");
    const auto result = RuleBasedBankGenerator{}.generate({structured});
    if (result.materials.size() != 1)
        return 1;
    if (result.questions.size() != 2 || !result.needsReviewQuestions.isEmpty())
        return 2;
    const QString materialId = result.materials.first().toObject().value("id").toString();
    for (const auto& value : result.questions)
        if (value.toObject().value("materialId").toString() != materialId)
            return 3;
    if (!result.questions.first()
             .toObject()
             .value("solution")
             .toString()
             .contains(QStringLiteral("散射")))
        return 9;
    QString validationError;
    if (!quizpane::validateBank(bankFor(result), &validationError))
        return 4;

    // 真实试卷常以“（一）阅读以下文字”而不是“材料一”标记资料题；文字
    // PDF 的图表还必须作为题库 assets 保留，不能因为该页有文字层就被丢弃。
    ExtractedDocument visualMaterial;
    visualMaterial.sourcePath = QStringLiteral("visual.pdf");
    visualMaterial.hasPageBoundaries = true;
    visualMaterial.pageImages.insert(1, QByteArrayLiteral("png-bytes"));
    visualMaterial.plainText = QStringLiteral("（一）阅读以下文字，完成各题。\n"
                                               "根据下列统计资料回答问题。\n"
                                               "1. 资料中的数值是多少？\nA. 一\nB. 二\n答案：A\n");
    const auto visualResult = RuleBasedBankGenerator{}.generate({visualMaterial});
    if (visualResult.materials.size() != 1 || visualResult.questions.size() != 1 ||
        visualResult.questions.first().toObject().value("materialId").toString().isEmpty() ||
        visualResult.materials.first().toObject().value("images").toArray().isEmpty() ||
        visualResult.assets.isEmpty())
        return 99;

    ExtractedDocument imageOptions;
    imageOptions.sourcePath = QStringLiteral("image-options.pdf");
    imageOptions.hasPageBoundaries = true;
    imageOptions.pageImages.insert(1, QByteArrayLiteral("png-bytes"));
    imageOptions.plainText = QStringLiteral("1. 请从图中 A、B、C、D 四个图形中选择填入问号处的一项。\n"
                                              "正确答案：C\n");
    const auto imageOptionsResult = RuleBasedBankGenerator{}.generate({imageOptions});
    if (imageOptionsResult.questions.size() != 1 || !imageOptionsResult.needsReviewQuestions.isEmpty())
        return 100;
    const QJsonObject imageQuestion = imageOptionsResult.questions.first().toObject();
    if (imageQuestion.value("options").toArray().size() != 4 ||
        imageQuestion.value("options").toArray().at(0).toObject().value("text").toString() !=
            QStringLiteral("图A") ||
        imageQuestion.value("answer").toObject().value("optionIds").toArray().first().toString() !=
            QStringLiteral("c") || imageQuestion.value("stemImage").toObject().isEmpty())
        return 101;

    ExtractedDocument inlineDocument;
    inlineDocument.sourcePath = QStringLiteral("inline.md");
    inlineDocument.plainText =
        QStringLiteral("1、C++ 中哪个是顺序容器？\n"
                       "A、std::vector\nB、std::mutex\nC、std::thread\nD、std::filesystem\n"
                       "【答案】A\n【解析】vector 提供连续存储。\n"
                       "2、这道题故意没有答案\nA、甲\nB、乙\n");
    const auto inlineResult = RuleBasedBankGenerator{}.generate({inlineDocument});
    if (inlineResult.questions.size() != 1 || inlineResult.needsReviewQuestions.size() != 1)
        return 5;
    const QJsonObject first = inlineResult.questions.first().toObject();
    if (!first.value("solution").toString().contains(QStringLiteral("连续存储")))
        return 6;
    const QString reason = inlineResult.needsReviewQuestions.first()
                               .toObject()
                               .value("review")
                               .toObject()
                               .value("reason")
                               .toString();
    if (!reason.contains(QStringLiteral("答案")))
        return 7;

    ExtractedDocument multiple;
    multiple.sourcePath = QStringLiteral("multiple.txt");
    multiple.plainText =
        QStringLiteral("1. 下列哪些是容器？\nA. vector\nB. map\nC. mutex\nD. thread\n答案：AB\n");
    const auto multipleResult = RuleBasedBankGenerator{}.generate({multiple});
    if (multipleResult.questions.size() != 1 ||
        !multipleResult.needsReviewQuestions.isEmpty() ||
        multipleResult.questions.first().toObject().value("type").toString() !=
            QStringLiteral("multiple_choice") ||
        multipleResult.questions.first().toObject().value("answer").toObject()
            .value("optionIds").toArray().size() != 2)
        return 8;

    ExtractedDocument trueFalse;
    trueFalse.sourcePath = QStringLiteral("boolean.txt");
    trueFalse.plainText = QStringLiteral("1. C++ 是编译型语言。\nA. 错误\nB. 正确\n答案：正确\n");
    const auto trueFalseResult = RuleBasedBankGenerator{}.generate({trueFalse});
    if (trueFalseResult.questions.size() != 1 ||
        trueFalseResult.questions.first().toObject().value("type").toString() !=
            QStringLiteral("true_false") ||
        trueFalseResult.questions.first()
                .toObject()
                .value("answer")
                .toObject()
                .value("optionIds")
                .toArray()
                .first()
                .toString() != QStringLiteral("b"))
        return 19;

    ExtractedDocument compactAnswers;
    compactAnswers.sourcePath = QStringLiteral("answer-card.txt");
    compactAnswers.plainText = QStringLiteral("1. 第一题\nA. 甲\nB. 乙\n"
                                              "2. 第二题\nA. 丙\nB. 丁\n"
                                              "答案\n1-2 AB\n");
    const auto compactResult = RuleBasedBankGenerator{}.generate({compactAnswers});
    if (compactResult.questions.size() != 2 || !compactResult.needsReviewQuestions.isEmpty())
        return 20;

    // 场景 3：阶段分组——阶段一题目 + 阶段一答案 + 阶段二题目 + 阶段二答案。
    // 旧逻辑只认第一个答案区头，会把阶段二的题目当成答案区文本吞掉。这里断言
    // 两阶段共 4 题全部识别，且答案各自归属正确。
    ExtractedDocument staged;
    staged.sourcePath = QStringLiteral("staged.txt");
    staged.plainText = QStringLiteral("1. 阶段一第一题\nA. 甲\nB. 乙\n"
                                      "2. 阶段一第二题\nA. 丙\nB. 丁\n"
                                      "参考答案\n1.A\n2.B\n"
                                      "3. 阶段二第一题\nA. 戊\nB. 己\n"
                                      "4. 阶段二第二题\nA. 庚\nB. 辛\n"
                                      "参考答案\n3.A\n4.B\n");
    const auto stagedResult = RuleBasedBankGenerator{}.generate({staged});
    if (stagedResult.questions.size() != 4 || !stagedResult.needsReviewQuestions.isEmpty())
        return 21;
    {
        const auto ids = [](const QJsonObject& q) {
            const QJsonArray arr = q.value("answer").toObject().value("optionIds").toArray();
            QStringList out;
            for (const auto& v : arr)
                out.append(v.toString());
            return out.join(u'.');
        };
        const auto findQ = [&](int number) -> QJsonObject {
            for (const auto& v : stagedResult.questions)
                if (v.toObject().value("id").toString().contains(
                        QStringLiteral("q%1-").arg(number)))
                    return v.toObject();
            return {};
        };
        if (ids(findQ(1)) != QStringLiteral("a") || ids(findQ(2)) != QStringLiteral("b") ||
            ids(findQ(3)) != QStringLiteral("a") || ids(findQ(4)) != QStringLiteral("b"))
            return 22;
        if (!quizpane::validateBank(bankFor(stagedResult), &validationError))
            return 23;
    }

    // 场景 2：选项内联进题干行——题号、题干、四个选项写在同一行，答案另起一行。
    ExtractedDocument inlineOptions;
    inlineOptions.sourcePath = QStringLiteral("inline-options.txt");
    inlineOptions.plainText = QStringLiteral(
        "1. 下列哪个是顺序容器 A. vector B. mutex C. thread D. filesystem\n答案：A\n");
    const auto inlineOptionsResult = RuleBasedBankGenerator{}.generate({inlineOptions});
    if (inlineOptionsResult.questions.size() != 1 ||
        !inlineOptionsResult.needsReviewQuestions.isEmpty())
        return 24;
    {
        const QJsonObject q = inlineOptionsResult.questions.first().toObject();
        if (q.value("options").toArray().size() != 4)
            return 25;
        if (q.value("answer").toObject().value("optionIds").toArray().first().toString() !=
            QStringLiteral("a"))
            return 26;
        if (!q.value("stem").toString().contains(QStringLiteral("顺序容器")))
            return 27;
    }

    // 场景 2（尾随答案）：题干与选项分行，答案写在最后一行末尾。
    ExtractedDocument trailing;
    trailing.sourcePath = QStringLiteral("trailing.txt");
    trailing.plainText =
        QStringLiteral("1. C++ 是编译型语言吗 A. 是 B. 否 答案：A\n");
    const auto trailingResult = RuleBasedBankGenerator{}.generate({trailing});
    if (trailingResult.questions.size() != 1 ||
        !trailingResult.needsReviewQuestions.isEmpty())
        return 28;
    if (trailingResult.questions.first()
            .toObject()
            .value("answer")
            .toObject()
            .value("optionIds")
            .toArray()
            .first()
            .toString() != QStringLiteral("a"))
        return 29;

    // 场景 C：选项用圈码 ①②③④ 而非字母。
    ExtractedDocument circled;
    circled.sourcePath = QStringLiteral("circled.txt");
    circled.plainText = QStringLiteral("1. 下列哪一个是蓝色\n① 蓝色\n② 红色\n③ 黄色\n④ 黑色\n答案：①\n");
    const auto circledResult = RuleBasedBankGenerator{}.generate({circled});
    if (circledResult.questions.size() != 1 || !circledResult.needsReviewQuestions.isEmpty())
        return 30;
    {
        const QJsonObject q = circledResult.questions.first().toObject();
        if (q.value("options").toArray().size() != 4)
            return 31;
        if (q.value("answer").toObject().value("optionIds").toArray().first().toString() !=
            QStringLiteral("a"))
            return 32;
    }

    // 场景 C：选项用括号数字 (1)(2)(3)(4)。
    ExtractedDocument parenNumeric;
    parenNumeric.sourcePath = QStringLiteral("paren.txt");
    parenNumeric.plainText = QStringLiteral(
        "1. 选出正确的项\n(1) 甲\n(2) 乙\n(3) 丙\n(4) 丁\n答案：(1)\n");
    const auto parenResult = RuleBasedBankGenerator{}.generate({parenNumeric});
    if (parenResult.questions.size() != 1 || !parenResult.needsReviewQuestions.isEmpty())
        return 33;
    if (parenResult.questions.first()
            .toObject()
            .value("answer")
            .toObject()
            .value("optionIds")
            .toArray()
            .first()
            .toString() != QStringLiteral("a"))
        return 34;

    // 场景 D：选择题答案直接写在题干末尾括号里（（A）），选项在后面分行。
    ExtractedDocument bracketAnswer;
    bracketAnswer.sourcePath = QStringLiteral("bracket.txt");
    bracketAnswer.plainText = QStringLiteral(
        "1. 下列哪个是顺序容器（A）\nA. vector\nB. mutex\nC. thread\nD. filesystem\n");
    const auto bracketResult = RuleBasedBankGenerator{}.generate({bracketAnswer});
    if (bracketResult.questions.size() != 1 || !bracketResult.needsReviewQuestions.isEmpty())
        return 35;
    if (bracketResult.questions.first()
            .toObject()
            .value("answer")
            .toObject()
            .value("optionIds")
            .toArray()
            .first()
            .toString() != QStringLiteral("a"))
        return 36;
    if (!bracketResult.questions.first()
             .toObject()
             .value("stem")
             .toString()
             .contains(QStringLiteral("顺序容器")))
        return 37;

    // 场景 H：答案区是 markdown 表格。
    ExtractedDocument tableAnswer;
    tableAnswer.sourcePath = QStringLiteral("table.txt");
    tableAnswer.plainText = QStringLiteral("1. 第一题\nA. 甲\nB. 乙\n"
                                           "2. 第二题\nA. 丙\nB. 丁\n"
                                           "答案\n"
                                           "题号 | 1 | 2\n"
                                           "答案 | A | B\n");
    const auto tableResult = RuleBasedBankGenerator{}.generate({tableAnswer});
    if (tableResult.questions.size() != 2 || !tableResult.needsReviewQuestions.isEmpty())
        return 38;
    {
        const auto findQ = [&](int number) -> QJsonObject {
            for (const auto& v : tableResult.questions)
                if (v.toObject().value("id").toString().contains(
                        QStringLiteral("q%1-").arg(number)))
                    return v.toObject();
            return {};
        };
        if (findQ(1).value("answer").toObject().value("optionIds").toArray().first().toString() !=
                QStringLiteral("a") ||
            findQ(2).value("answer").toObject().value("optionIds").toArray().first().toString() !=
                QStringLiteral("b"))
            return 39;
    }

    // 场景 I：题干跨多行，中间夹空行续写，然后才出选项。
    ExtractedDocument multilineStem;
    multilineStem.sourcePath = QStringLiteral("multiline.txt");
    multilineStem.plainText = QStringLiteral(
        "1. 这是一道题干很长的问题，第一行\n"
        "题干继续第二行\n"
        "\n"
        "题干第三行（空行之后续写）\n"
        "A. 甲\nB. 乙\n答案：A\n");
    const auto multilineResult = RuleBasedBankGenerator{}.generate({multilineStem});
    if (multilineResult.questions.size() != 1 || !multilineResult.needsReviewQuestions.isEmpty())
        return 40;
    {
        const QString stem = multilineResult.questions.first()
                                 .toObject()
                                 .value("stem")
                                 .toString();
        if (!stem.contains(QStringLiteral("第一行")) ||
            !stem.contains(QStringLiteral("第二行")) ||
            !stem.contains(QStringLiteral("第三行")))
            return 41;
    }

    // 场景 C（答案区）：选项用圈码，答案也用圈码，且答案单独成区（参考答案）。
    ExtractedDocument circledSection;
    circledSection.sourcePath = QStringLiteral("circled-section.txt");
    circledSection.plainText = QStringLiteral(
        "1. 下列哪一个是蓝色\n① 蓝色\n② 红色\n③ 黄色\n④ 黑色\n"
        "2. 下列哪一个是红色\n① 蓝色\n② 红色\n③ 黄色\n④ 黑色\n"
        "参考答案\n1.①\n2.②\n");
    const auto circledSectionResult = RuleBasedBankGenerator{}.generate({circledSection});
    if (circledSectionResult.questions.size() != 2 ||
        !circledSectionResult.needsReviewQuestions.isEmpty())
        return 42;
    {
        const auto findQ = [&](int number) -> QJsonObject {
            for (const auto& v : circledSectionResult.questions)
                if (v.toObject().value("id").toString().contains(
                        QStringLiteral("q%1-").arg(number)))
                    return v.toObject();
            return {};
        };
        if (findQ(1).value("answer").toObject().value("optionIds").toArray().first().toString() !=
                QStringLiteral("a") ||
            findQ(2).value("answer").toObject().value("optionIds").toArray().first().toString() !=
                QStringLiteral("b"))
            return 43;
    }

    // 规则候选最终仍进入现有声明式 ZIP，不产生本机代码。这里复用上面以内存
    // 构造的稳定样例写包、检查 manifest、重新读 ZIP，再交给运行时加载。
    // 测试因此不再依赖开发机 build/ 目录中的临时文件，干净检出也能完整运行。
    const QJsonObject generatedBank = bankFor(result);
    QTemporaryDir packageDirectory;
    if (!packageDirectory.isValid())
        return 13;
    const QJsonObject manifest{{"manifestVersion", 2},
                               {"id", "local.rules.test"},
                               {"name", "Rules"},
                               {"version", "1.0.0"},
                               {"kind", "declarative"},
                               {"runtime", QJsonObject{{"format", "quizpane.bank+json"},
                                                       {"schemaVersion", 2},
                                                       {"entry", "content/bank.json"}}},
                               {"permissions", QJsonObject{{"network", false}}}};
    const QString packagePath =
        packageDirectory.filePath(QStringLiteral("rules.quizpane-provider"));
    if (!quizpane::writeZipArchive(packagePath,
                                   {{QStringLiteral("manifest.json"),
                                     QJsonDocument(manifest).toJson(QJsonDocument::Compact)},
                                    {QStringLiteral("content/bank.json"),
                                     QJsonDocument(generatedBank).toJson(QJsonDocument::Compact)}},
                                   &validationError))
        return 14;
    quizpane::ProviderPackageInfo packageInfo;
    if (!quizpane::ProviderInstaller{}.inspect(packagePath, &packageInfo, &validationError))
        return 15;
    quizpane::ZipArchiveReader archive(packagePath);
    QTemporaryDir extracted;
    if (!archive.isReadable() || !extracted.isValid() ||
        !QDir().mkpath(extracted.filePath(QStringLiteral("content"))))
        return 16;
    QFile manifestFile(extracted.filePath(QStringLiteral("manifest.json")));
    QFile bankFile(extracted.filePath(QStringLiteral("content/bank.json")));
    if (!manifestFile.open(QIODevice::WriteOnly) || !bankFile.open(QIODevice::WriteOnly))
        return 17;
    manifestFile.write(archive.fileData(QStringLiteral("manifest.json")));
    bankFile.write(archive.fileData(QStringLiteral("content/bank.json")));
    manifestFile.close();
    bankFile.close();
    quizpane::DeclarativeProvider provider;
    if (!provider.load(bankFile.fileName(), &validationError))
        return 18;
    return 0;
}
