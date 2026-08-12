#include "quizpane/studio/generation_workflow.hpp"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextStream>

namespace {

bool parsePages(const QString& value, int* first, int* last) {
    static const QRegularExpression pattern(QStringLiteral(R"(^\s*(\d+)\s*[-:]\s*(\d+)\s*$)"));
    const auto match = pattern.match(value);
    if (!match.hasMatch())
        return false;
    *first = match.captured(1).toInt();
    *last = match.captured(2).toInt();
    return *first > 0 && *last >= *first;
}

bool writeOutput(const QByteArray& bytes, const QString& path) {
    if (path.isEmpty()) {
        QTextStream(stdout) << bytes;
        return true;
    }
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("quizpane-pdf-recognizer"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "开发者回测工具：调用题库制作器 GUI 的同一生产识别入口。"));
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("pdf"), QStringLiteral("要回测的 PDF"));
    const QCommandLineOption pagesOption(QStringList{QStringLiteral("p"), QStringLiteral("pages")},
        QStringLiteral("只把指定原文页码范围送入生产识别入口，例如 5-30"),
        QStringLiteral("first-last"));
    const QCommandLineOption paddingOption(QStringList{QStringLiteral("padding")},
        QStringLiteral("在指定范围两侧外扩页数（默认 2）"), QStringLiteral("pages"),
        QStringLiteral("2"));
    const QCommandLineOption noAnswer(QStringList{QStringLiteral("no-answer")},
        QStringLiteral("使用 GUI 的无答案题库模式"));
    const QCommandLineOption outputOption(QStringList{QStringLiteral("o"), QStringLiteral("output")},
        QStringLiteral("将回测 JSON 写入文件；默认输出到 stdout"), QStringLiteral("file"));
    parser.addOptions({pagesOption, paddingOption, noAnswer, outputOption});
    parser.process(app);
    const QStringList positional = parser.positionalArguments();
    if (positional.size() != 1)
        parser.showHelp(2);

    bool paddingOk = false;
    const int padding = parser.value(paddingOption).toInt(&paddingOk);
    if (!paddingOk || padding < 0) {
        qCritical("--padding 必须是非负整数");
        return 2;
    }
    quizpane::studio::PdfExtractionRange range;
    if (parser.isSet(pagesOption)) {
        if (!parsePages(parser.value(pagesOption), &range.firstPage, &range.lastPage)) {
            qCritical("--pages 格式应为 first-last，且 first <= last");
            return 2;
        }
        range.padding = padding;
    } else if (parser.isSet(paddingOption)) {
        qCritical("--padding 只能与 --pages 一起使用");
        return 2;
    }

    const QString path = QFileInfo(positional.first()).absoluteFilePath();
    const bool hasAnswerKey = !parser.isSet(noAnswer);
    // 这是 CLI 唯一的识别调用。题号、选项、答案、材料、OCR、风险审计和错误语义
    // 全部由 GUI 同样调用的 runRuleBasedGeneration() 决定，CLI 不做任何后处理。
    const quizpane::studio::RuleBasedRunResult run =
        quizpane::studio::runRuleBasedGeneration({{path, {}, hasAnswerKey, range, {}}});
    if (!run.error.isEmpty()) {
        qCritical().noquote() << run.error;
        return 3;
    }
    const auto& generated = run.candidate;
    QJsonObject report{{"source", path},
                       {"requestedRange", QJsonObject{{"first", range.firstPage},
                                                       {"last", range.lastPage},
                                                       {"padding", range.padding}}},
                       {"hasAnswerKey", generated.hasAnswerKey},
                       {"materials", generated.materials},
                       {"questions", generated.questions},
                       {"needsReviewQuestions", generated.needsReviewQuestions},
                       {"warnings", QJsonArray::fromStringList(generated.warnings)},
                       {"summary", QJsonObject{{"questions", generated.questions.size()},
                                               {"needsReview", generated.needsReviewQuestions.size()},
                                               {"materials", generated.materials.size()},
                                               {"assets", generated.assets.size()}}}};
    if (!writeOutput(QJsonDocument(report).toJson(QJsonDocument::Indented),
                     parser.value(outputOption))) {
        qCritical("无法写入输出文件");
        return 4;
    }
    return generated.questions.isEmpty() && generated.needsReviewQuestions.isEmpty() ? 5 : 0;
}
