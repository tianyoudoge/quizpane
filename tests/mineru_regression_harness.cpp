// MinerU 真卷回归 harness。
//
// 与 pdf_regression_harness 同一定位：需要调用方自备真实试卷的 MinerU 解析
// 结果，因而刻意不注册进 CTest。真题多为第三方版权材料，不随仓库分发；
// 单元测试用 tests/fixtures/mineru-layout-fixture.json 这份合成夹具覆盖结构。
//
// 用法：
//   mineru_regression_harness <layout.json 或 result.zip> <output-dir> [source.pdf]
//
// 它把 MinerU 输出适配成 ExtractedDocument 后直接喂给现有规则引擎，打印锚点
// 数量与生成结果，并把题库 JSON 落到 output-dir，供人工比对与 golden 归档。

#include "quizpane/studio/mineru_output_adapter.hpp"
#include "quizpane/studio/rule_based_generator.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

int totalAnchors(const QHash<int, QList<quizpane::studio::PdfTextAnchor>>& anchors) {
    int total = 0;
    for (auto it = anchors.constBegin(); it != anchors.constEnd(); ++it)
        total += it.value().size();
    return total;
}

bool writeFile(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().size() < 3 || app.arguments().size() > 4) {
        qCritical("usage: mineru_regression_harness <layout.json|result.zip> <output-dir> [source.pdf]");
        return 2;
    }

    const QString input = app.arguments().at(1);
    const QString outputDir = app.arguments().at(2);
    const QString sourcePath = app.arguments().size() == 4
        ? QFileInfo(app.arguments().at(3)).absoluteFilePath()
        : QFileInfo(input).absolutePath() + QStringLiteral("/source.pdf");

    using namespace quizpane::studio;
    const MineruAdaptResult adapted =
        input.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive)
            ? adaptMineruZip(input, sourcePath)
            : [&] {
                  QFile file(input);
                  if (!file.open(QIODevice::ReadOnly)) {
                      MineruAdaptResult failure;
                      failure.error = QStringLiteral("无法读取 %1").arg(input);
                      return failure;
                  }
                  return adaptMineruLayout(file.readAll(), sourcePath);
              }();

    if (!adapted.error.isEmpty()) {
        qCritical().noquote() << adapted.error;
        return 3;
    }

    const ExtractedDocument& document = adapted.document;
    qInfo().noquote() << QStringLiteral("backend=%1 version=%2 pages=%3")
                             .arg(adapted.backend, adapted.versionName)
                             .arg(document.plainText.split(QChar(u'\f')).size());
    qInfo().noquote() << QStringLiteral("anchors: question=%1 option=%2 line=%3")
                             .arg(totalAnchors(document.questionAnchors))
                             .arg(totalAnchors(document.optionLabelAnchors))
                             .arg(totalAnchors(document.lineAnchors));
    for (const QString& warning : document.warnings)
        qInfo().noquote() << QStringLiteral("warning: ") + warning;

    RuleBasedBankGenerator generator;
    const RuleBasedGenerationResult result = generator.generate({document}, true);
    qInfo().noquote() << QStringLiteral("generated: questions=%1 needsReview=%2 materials=%3")
                             .arg(result.questions.size())
                             .arg(result.needsReviewQuestions.size())
                             .arg(result.materials.size());
    for (const QString& warning : result.warnings)
        qInfo().noquote() << QStringLiteral("rule warning: ") + warning;

    for (const QJsonValue& value : result.needsReviewQuestions) {
        const QJsonObject question = value.toObject();
        qInfo().noquote() << QStringLiteral("review: %1 | %2")
                                 .arg(question.value(QStringLiteral("stem")).toString().left(28),
                                      question.value(QStringLiteral("review"))
                                          .toObject()
                                          .value(QStringLiteral("reason"))
                                          .toString());
    }

    QJsonObject bank;
    bank.insert(QStringLiteral("questions"), result.questions);
    bank.insert(QStringLiteral("materials"), result.materials);
    bank.insert(QStringLiteral("needsReviewQuestions"), result.needsReviewQuestions);
    const QString bankPath = QDir(outputDir).filePath(QStringLiteral("mineru-bank.json"));
    if (!writeFile(bankPath, QJsonDocument(bank).toJson(QJsonDocument::Indented))) {
        qCritical().noquote() << QStringLiteral("无法写出 %1").arg(bankPath);
        return 4;
    }
    const QString textPath = QDir(outputDir).filePath(QStringLiteral("mineru-plain.txt"));
    if (!writeFile(textPath, document.plainText.toUtf8())) {
        qCritical().noquote() << QStringLiteral("无法写出 %1").arg(textPath);
        return 4;
    }
    qInfo().noquote() << QStringLiteral("written: %1").arg(bankPath);
    return 0;
}
