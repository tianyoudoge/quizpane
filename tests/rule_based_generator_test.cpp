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
            {"questions", result.normalQuestions}};
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
    if (result.normalQuestions.size() != 2 || !result.needsReviewQuestions.isEmpty())
        return 2;
    const QString materialId = result.materials.first().toObject().value("id").toString();
    for (const auto& value : result.normalQuestions)
        if (value.toObject().value("materialId").toString() != materialId)
            return 3;
    if (!result.normalQuestions.first()
             .toObject()
             .value("solution")
             .toString()
             .contains(QStringLiteral("散射")))
        return 9;
    QString validationError;
    if (!quizpane::validateBank(bankFor(result), &validationError))
        return 4;

    ExtractedDocument inlineDocument;
    inlineDocument.sourcePath = QStringLiteral("inline.md");
    inlineDocument.plainText =
        QStringLiteral("1、C++ 中哪个是顺序容器？\n"
                       "A、std::vector\nB、std::mutex\nC、std::thread\nD、std::filesystem\n"
                       "【答案】A\n【解析】vector 提供连续存储。\n"
                       "2、这道题故意没有答案\nA、甲\nB、乙\n");
    const auto inlineResult = RuleBasedBankGenerator{}.generate({inlineDocument});
    if (inlineResult.normalQuestions.size() != 1 || inlineResult.needsReviewQuestions.size() != 1)
        return 5;
    const QJsonObject first = inlineResult.normalQuestions.first().toObject();
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
    if (!multipleResult.normalQuestions.isEmpty() ||
        multipleResult.needsReviewQuestions.size() != 1)
        return 8;

    ExtractedDocument trueFalse;
    trueFalse.sourcePath = QStringLiteral("boolean.txt");
    trueFalse.plainText = QStringLiteral("1. C++ 是编译型语言。\nA. 错误\nB. 正确\n答案：正确\n");
    const auto trueFalseResult = RuleBasedBankGenerator{}.generate({trueFalse});
    if (trueFalseResult.normalQuestions.size() != 1 ||
        trueFalseResult.normalQuestions.first().toObject().value("type").toString() !=
            QStringLiteral("true_false") ||
        trueFalseResult.normalQuestions.first()
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
    if (compactResult.normalQuestions.size() != 2 || !compactResult.needsReviewQuestions.isEmpty())
        return 20;

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
