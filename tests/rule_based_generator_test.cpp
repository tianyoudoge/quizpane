#include "quizpane/bank_validator.hpp"
#include "quizpane/declarative_provider.hpp"
#include "quizpane/provider_installer.hpp"
#include "quizpane/studio/rule_based_generator.hpp"
#include "quizpane/zip_archive.hpp"

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPen>
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

    // 无答案模式只导入题干与选项：不能因为没有答案被打回复核，也不能把空答案
    // 伪装成可判分题目。原始小题号须随 source 保留。
    ExtractedDocument answerless;
    answerless.sourcePath = QStringLiteral("answerless.txt");
    answerless.plainText = QStringLiteral("76. 下列说法正确的是？\nA. 甲\nB. 乙\n");
    const auto answerlessResult = RuleBasedBankGenerator{}.generate({answerless}, false);
    if (answerlessResult.hasAnswerKey || answerlessResult.questions.size() != 1 ||
        !answerlessResult.needsReviewQuestions.isEmpty()) return 115;
    const QJsonObject answerlessQuestion = answerlessResult.questions.first().toObject();
    if (answerlessQuestion.contains("answer") || answerlessQuestion.contains("solution") ||
        answerlessQuestion.value("source").toObject().value("questionNumber").toInt() != 76)
        return 116;
    const QJsonObject answerlessBank{{"schemaVersion", 3}, {"answerPolicy", "none"},
        {"title", "No answer"}, {"catalogs", QJsonArray{QJsonObject{{"id", "generated"},
            {"title", "No answer"}, {"practice", QJsonObject{{"mode", "all"}}}}}},
        {"questions", answerlessResult.questions}};
    if (!quizpane::validateBank(answerlessBank, &validationError)) return 117;

    // 粉笔等网页导出的 PDF 可能把题干排在裸题号之前，并把无题号的“正确答案”
    // 集中排在题目之前。生成器需要把题干折回题号、且仅在答案数与题数完全相同
    // 时按顺序配对，不能让整页题目因视觉阅读顺序而全部丢失。
    ExtractedDocument trailingNumbers;
    trailingNumbers.sourcePath = QStringLiteral("web-export.pdf");
    trailingNumbers.plainText = QStringLiteral(
        "正确答案： C\n你的答案：\n正确答案： A\n你的答案：\n"
        "第一题的题干在题号前。\n1.\nA. 甲 B. 乙 C. 丙 D. 丁\n"
        "第二题的题干也在题号前。\n2.\nA. 一 B. 二 C. 三 D. 四\n");
    const auto trailingNumberResult = RuleBasedBankGenerator{}.generate({trailingNumbers});
    if (trailingNumberResult.questions.size() != 2) return 121;
    const QJsonObject trailingFirst = trailingNumberResult.questions.first().toObject();
    if (!trailingFirst.value("stem").toString().contains(QStringLiteral("第一题")) ||
        trailingFirst.value("answer").toObject().value("optionIds").toArray().first().toString()
            != QStringLiteral("c")) return 122;

    // PDF 文字层会漏掉独立绘制的下划线；资料解析必须保留【甲】等原题编号，
    // 并将纯横线补成可渲染 token，随材料携带裁切后的原卷版式附件。
    ExtractedDocument fillLayout;
    fillLayout.sourcePath = QStringLiteral("fill-layout.pdf");
    fillLayout.hasPageBoundaries = true;
    fillLayout.plainText = QStringLiteral(
        "材料一：阅读下面文字\n这是【甲】需要填写的 。\n根据材料回答1-1题。\n"
        "1. 第 1 段中划线词语使用正确的是？\nA. 甲\nB. 乙\n答案：A\n");
    fillLayout.lineAnchors.insert(1, {
        {QStringLiteral("材料一：阅读下面文字"), QRectF(0.1, 0.45, 0.4, 0.03)},
        {QStringLiteral("1. 第 1 段中划线词语使用正确的是？"), QRectF(0.1, 0.75, 0.4, 0.03)}});
    // 下划线范围来自 PDF 文字框与页面细线的交叉检测，生成器仅转交其精确字符
    // offset；不能根据【甲】样式或后续题目选项反向猜测。
    fillLayout.underlineDecorations.insert(1, {
        {QStringLiteral("这是【甲】需要填写的 。"), {{5, 2}}, QRectF(0.1, 0.50, 0.4, 0.03)}});
    fillLayout.questionAnchors.insert(1, {
        {QStringLiteral("1"), QRectF(0.1, 0.75, 0.02, 0.02)}});
    QImage fillPage(800, 1200, QImage::Format_ARGB32_Premultiplied);
    fillPage.fill(Qt::white);
    QByteArray fillPng;
    QBuffer fillBuffer(&fillPng);
    if (!fillBuffer.open(QIODevice::WriteOnly) || !fillPage.save(&fillBuffer, "PNG")) return 90;
    fillLayout.pageImages.insert(1, fillPng);
    const auto fillResult = RuleBasedBankGenerator{}.generate({fillLayout});
    if (fillResult.materials.size() != 1 || fillResult.assets.isEmpty() ||
        fillResult.materials.first().toObject().value("images").toArray().isEmpty() ||
        !fillResult.materials.first().toObject().value("body").toString().contains(
            QStringLiteral("【甲】")) ||
        !fillResult.materials.first().toObject().value("body").toString().contains(
            QStringLiteral("〔填空〕")) ||
        !fillResult.questions.first().toObject().value("review").toObject().value("signals")
            .toArray().contains(QStringLiteral("material-layout:underline-or-blank")))
        return 91;
    const QJsonArray detectedUnderlines = fillResult.materials.first().toObject()
        .value("underlines").toArray();
    if (detectedUnderlines.size() != 1 || detectedUnderlines.first().toObject()
        .value("start").toInt(-1) != 5 || detectedUnderlines.first().toObject()
        .value("length").toInt() != 2)
        return 120;

    // PDF 的文字 API 常把每一条视觉行都输出成一行；材料正文要根据行坐标和
    // OCR 保留下来的空行还原自然段，而不是在复核页显示成每行一个段落。
    ExtractedDocument wrappedMaterial;
    wrappedMaterial.sourcePath = QStringLiteral("wrapped-material.pdf");
    wrappedMaterial.hasPageBoundaries = true;
    wrappedMaterial.plainText = QStringLiteral(
        "材料一：阅读下面文字\n第一自然段的前半句，\n紧接着是同一段的后半句。\n\n"
        "第二自然段的第一行，\n第二自然段的第二行。\n根据材料回答1-1题。\n"
        "1. 文意理解正确的是？\nA. 甲\nB. 乙\n答案：A\n");
    // QPdfDocument 在部分 PDF 中输出 CRLF；它仍是一条视觉行，不能被误判为
    // “一条文本行 + 一个空行”。
    wrappedMaterial.plainText.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));
    wrappedMaterial.lineAnchors.insert(1, {
        {QStringLiteral("材料一：阅读下面文字"), QRectF(0.10, 0.10, 0.35, 0.02)},
        {QStringLiteral("第一自然段的前半句，"), QRectF(0.10, 0.16, 0.30, 0.02)},
        {QStringLiteral("紧接着是同一段的后半句。"), QRectF(0.10, 0.19, 0.34, 0.02)},
        {QStringLiteral("第二自然段的第一行，"), QRectF(0.13, 0.25, 0.30, 0.02)},
        {QStringLiteral("第二自然段的第二行。"), QRectF(0.10, 0.28, 0.30, 0.02)},
        {QStringLiteral("根据材料回答1-1题。"), QRectF(0.10, 0.34, 0.30, 0.02)},
        {QStringLiteral("1. 文意理解正确的是？"), QRectF(0.10, 0.45, 0.32, 0.02)}});
    const auto wrappedResult = RuleBasedBankGenerator{}.generate({wrappedMaterial});
    if (wrappedResult.materials.size() != 1) return 118;
    const QString wrappedBody = wrappedResult.materials.first().toObject().value("body").toString();
    if (!wrappedBody.contains(QStringLiteral("第一自然段的前半句，紧接着是同一段的后半句。")) ||
        !wrappedBody.contains(QStringLiteral("第二自然段的第一行，第二自然段的第二行。")) ||
        !wrappedBody.contains(QStringLiteral("后半句。\n\n第二自然段"))) return 119;

    // 题号锚点可能因“44、第…”这类版式而取不到。裁切必须改用完整题干的
    // 行锚点；否则宁可没有截图，也不能把第 44 题的题干裁进材料图。
    ExtractedDocument safeCrop;
    safeCrop.sourcePath = QStringLiteral("safe-crop.pdf");
    safeCrop.hasPageBoundaries = true;
    safeCrop.plainText = QStringLiteral("阅读以下文字，完成各题。\n材料正文。\n"
                                         "44、第 1 段意在说明：\nA、甲\nB、乙\n答案：A\n");
    safeCrop.lineAnchors.insert(1, {
        {QStringLiteral("阅读以下文字，完成各题。"), QRectF(0.1, 0.20, 0.5, 0.03)},
        {QStringLiteral("44、第 1 段意在说明："), QRectF(0.1, 0.65, 0.5, 0.03)}});
    safeCrop.pageImages.insert(1, fillPng);
    const auto safeCropResult = RuleBasedBankGenerator{}.generate({safeCrop});
    if (safeCropResult.materials.size() != 1)
        return 92;
    const QString safeCropPath = safeCropResult.materials.first().toObject().value("images")
        .toArray().first().toObject().value("path").toString();
    const QImage safeCropImage = QImage::fromData(safeCropResult.assets.value(safeCropPath), "PNG");
    // 源页 1200px，高度上界为题干 y=0.65 减边距，截图不能延伸到题干行。
    if (safeCropImage.isNull())
        return 93;
    if (safeCropImage.height() >= 1000)
        return 94;

    // 题干明确“如图所示”时，即便 A-D 文本选项都已完整解析，也必须保留题干
    // 与选项行之间的插图。它不是图形选项，不能按选项行上方的窄条公式区域裁切。
    ExtractedDocument illustratedStem;
    illustratedStem.sourcePath = QStringLiteral("illustrated-stem.pdf");
    illustratedStem.hasPageBoundaries = true;
    illustratedStem.plainText = QStringLiteral(
        "1. 小王站在何处？如图所示。\nA. 30 米\nB. 40 米\nC. 50 米\nD. 60 米\n答案：C\n");
    illustratedStem.questionAnchors.insert(1, {
        {QStringLiteral("1"), QRectF(0.08, 0.10, 0.02, 0.025)}});
    illustratedStem.lineAnchors.insert(1, {
        {QStringLiteral("1. 小王站在何处？如图所示。"), QRectF(0.08, 0.10, 0.62, 0.055)}});
    illustratedStem.optionLabelAnchors.insert(1, {
        {QStringLiteral("a"), QRectF(0.10, 0.63, 0.02, 0.02)},
        {QStringLiteral("b"), QRectF(0.34, 0.63, 0.02, 0.02)},
        {QStringLiteral("c"), QRectF(0.58, 0.63, 0.02, 0.02)},
        {QStringLiteral("d"), QRectF(0.82, 0.63, 0.02, 0.02)}});
    QImage illustratedPage(800, 1200, QImage::Format_ARGB32_Premultiplied);
    illustratedPage.fill(Qt::white);
    {
        QPainter painter(&illustratedPage);
        painter.setPen(QPen(Qt::black, 7));
        painter.drawLine(90, 610, 710, 610);
        painter.drawLine(400, 610, 400, 280); // 题干插图的高线，必须不能被截掉。
    }
    QByteArray illustratedPng;
    QBuffer illustratedBuffer(&illustratedPng);
    if (!illustratedBuffer.open(QIODevice::WriteOnly) || !illustratedPage.save(&illustratedBuffer, "PNG"))
        return 123;
    illustratedStem.pageImages.insert(1, illustratedPng);
    const auto illustratedResult = RuleBasedBankGenerator{}.generate({illustratedStem});
    if (illustratedResult.questions.size() != 1) return 124;
    const QJsonObject illustratedQuestion = illustratedResult.questions.first().toObject();
    const QJsonObject illustratedAsset = illustratedQuestion.value("stemImage").toObject();
    const QJsonObject illustratedCrop = illustratedAsset.value("autoCrop").toObject();
    const QString illustratedPath = illustratedAsset.value("path").toString();
    if (illustratedPath.isEmpty() || !illustratedResult.assets.contains(illustratedPath)) return 125;
    if (!illustratedAsset.value("alt").toString().contains(QStringLiteral("题干插图"))) return 126;
    if (illustratedCrop.value("y").toDouble() > 0.17) return 127;
    if (illustratedCrop.value("height").toDouble() < 0.42) return 128;
    if (illustratedCrop.value("y").toDouble() + illustratedCrop.value("height").toDouble() > 0.63)
        return 129;

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

    // 资料分析正文可在上一页而统计图紧挨 116 位于下一页。正文已有结构化文本，
    // 视觉附件只应保留图表页，不能再把上一页正文重复截图。
    ExtractedDocument crossPageMaterial;
    crossPageMaterial.sourcePath = QStringLiteral("cross-page.pdf");
    crossPageMaterial.hasPageBoundaries = true;
    QImage prosePage(800, 1200, QImage::Format_ARGB32_Premultiplied);
    prosePage.fill(Qt::white);
    QImage chartPage(800, 1200, QImage::Format_ARGB32_Premultiplied);
    chartPage.fill(Qt::white);
    {
        QPainter painter(&chartPage);
        painter.setPen(QPen(Qt::black, 3));
        painter.drawLine(120, 120, 120, 700);
        painter.drawLine(120, 700, 700, 700);
    }
    const auto pngFor = [](const QImage& image) {
        QByteArray png;
        QBuffer buffer(&png);
        if (buffer.open(QIODevice::WriteOnly)) image.save(&buffer, "PNG");
        return png;
    };
    crossPageMaterial.pageImages.insert(1, pngFor(prosePage));
    crossPageMaterial.pageImages.insert(2, pngFor(chartPage));
    crossPageMaterial.plainText = QStringLiteral(
        "五、资料分析，共 20 题\n（一）\n根据下列统计资料回答问题。\n"
        "2009 年固定资产投资数据。\f"
        "116. 2004 年全年投资额为：\nA. 100\nB. 200\n答案：A\n");
    crossPageMaterial.lineAnchors.insert(1, {
        {QStringLiteral("（一）"), QRectF(0.1, 0.10, 0.2, 0.03)}});
    crossPageMaterial.lineAnchors.insert(2, {
        {QStringLiteral("116. 2004 年全年投资额为："), QRectF(0.1, 0.75, 0.6, 0.03)}});
    crossPageMaterial.questionAnchors.insert(2, {
        {QStringLiteral("116"), QRectF(0.1, 0.75, 0.04, 0.02)}});
    const auto crossPageResult = RuleBasedBankGenerator{}.generate({crossPageMaterial});
    if (crossPageResult.materials.size() != 1 || crossPageResult.questions.size() != 1 ||
        crossPageResult.materials.first().toObject().value("images").toArray().size() != 1 ||
        crossPageResult.assets.size() != 1 ||
        crossPageResult.questions.first().toObject().value("materialId").toString() !=
            crossPageResult.materials.first().toObject().value("id").toString() ||
        crossPageResult.questions.first().toObject().contains("stemImage"))
        return 106;

    // 资料分析的题目文字通常可靠且非常短；应只把共享材料标为软复核，避免
    // 同一份图表下的每道子题都被重复打回复核列表。
    {
        const QJsonObject dataAnalysisReview =
            crossPageResult.materials.first().toObject().value("review").toObject();
        if (!dataAnalysisReview.value("needsReview").toBool() ||
            dataAnalysisReview.value("riskLevel").toString() != QStringLiteral("soft") ||
            !dataAnalysisReview.value("signals").toArray().contains(
                QStringLiteral("material-type:资料分析")))
            return 111;
        if (crossPageResult.questions.first().toObject().value("review").toObject()
                .value("signals").toArray().contains(QStringLiteral("material-type:资料分析")))
            return 114;
    }

    // 图形推理整题型分区：即使规则解析完全成功，也要标注 soft 风险，因为规则
    // 无法验证图形规律本身是否正确。
    ExtractedDocument graphicalReasoning;
    graphicalReasoning.sourcePath = QStringLiteral("graphical-reasoning.txt");
    graphicalReasoning.plainText = QStringLiteral(
        "四、图形推理，共 5 题\n"
        "1. 下列哪一项符合规律？\nA. 甲\nB. 乙\nC. 丙\nD. 丁\n答案：B\n");
    const auto graphicalReasoningResult =
        RuleBasedBankGenerator{}.generate({graphicalReasoning});
    if (graphicalReasoningResult.questions.size() != 1 ||
        !graphicalReasoningResult.needsReviewQuestions.isEmpty())
        return 112;
    {
        const QJsonObject graphicalReview =
            graphicalReasoningResult.questions.first().toObject().value("review").toObject();
        if (!graphicalReview.value("needsReview").toBool() ||
            graphicalReview.value("riskLevel").toString() != QStringLiteral("soft") ||
            !graphicalReview.value("signals").toArray().contains(
                QStringLiteral("material-type:图形推理")))
            return 113;
    }

    // “根据下列材料”与“根据下列统计资料”都是独立资料头；相邻资料组不能
    // 合并，否则后组图表会丢失，前组最后一道题的 D 选项也会吞入新资料标题。
    ExtractedDocument adjacentMaterials;
    adjacentMaterials.sourcePath = QStringLiteral("adjacent-materials.txt");
    adjacentMaterials.plainText = QStringLiteral(
        "（一）\n根据下列统计资料回答问题。\n第一份资料。\n"
        "1. 第一题\nA. 甲\nB. 乙\n答案：A\n"
        "（二）\n根据下列材料完成各题。\n第二份资料。\n"
        "2. 第二题\nA. 丙\nB. 丁\n答案：B\n");
    const auto adjacentResult = RuleBasedBankGenerator{}.generate({adjacentMaterials});
    if (adjacentResult.materials.size() != 2 || adjacentResult.questions.size() != 2 ||
        adjacentResult.questions.at(0).toObject().value("materialId").toString() ==
            adjacentResult.questions.at(1).toObject().value("materialId").toString() ||
        adjacentResult.questions.at(0).toObject().value("options").toArray().at(1).toObject()
            .value("text").toString() != QStringLiteral("乙"))
        return 107;

    // “如图/表”只是文字题干的一部分时，文字层已经有完整选项，不能把整页 PDF
    // 误当题干图片。否则阅读材料页会在每一道子题下重复显示。
    ExtractedDocument textualVisualStem;
    textualVisualStem.sourcePath = QStringLiteral("textual-visual-stem.pdf");
    textualVisualStem.hasPageBoundaries = true;
    textualVisualStem.pageImages.insert(1, QByteArrayLiteral("page-image"));
    textualVisualStem.plainText = QStringLiteral(
        "1. 如图所示，哪一种现象正确？\nA. 甲\nB. 乙\nC. 丙\nD. 丁\n答案：A\n");
    const auto textualVisualStemResult = RuleBasedBankGenerator{}.generate({textualVisualStem});
    if (textualVisualStemResult.questions.size() != 1 ||
        textualVisualStemResult.questions.first().toObject().contains("stemImage"))
        return 107;

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

    // 图形、公式等选项在 PDF 里可能没有文字层，只有 A/B/C/D 标签。PDF 的
    // 文字坐标和实际绘制位置可能不一致，因此保留“本题到下一题前”的完整题图，
    // 不生成可能夹带题号或相邻题目的四张错误小图。
    ExtractedDocument formulaImages;
    formulaImages.sourcePath = QStringLiteral("formula-options.pdf");
    formulaImages.hasPageBoundaries = true;
    formulaImages.plainText = QStringLiteral(
        "76. 请从下列图形中选择填入问号处的一项。\nA、 B、 C、 D、\n正确答案：C\n");
    formulaImages.questionAnchors.insert(1, {{QStringLiteral("76"), QRectF(0.1, 0.2, 0.02, 0.02)}});
    formulaImages.optionLabelAnchors.insert(1, {
        {QStringLiteral("a"), QRectF(0.18, 0.55, 0.02, 0.02)},
        {QStringLiteral("b"), QRectF(0.38, 0.55, 0.02, 0.02)},
        {QStringLiteral("c"), QRectF(0.58, 0.55, 0.02, 0.02)},
        {QStringLiteral("d"), QRectF(0.78, 0.55, 0.02, 0.02)}});
    QImage formulaPage(800, 1200, QImage::Format_ARGB32_Premultiplied);
    formulaPage.fill(Qt::white);
    QByteArray formulaPng;
    QBuffer formulaBuffer(&formulaPng);
    if (!formulaBuffer.open(QIODevice::WriteOnly) || !formulaPage.save(&formulaBuffer, "PNG")) return 102;
    formulaImages.pageImages.insert(1, formulaPng);
    const auto formulaResult = RuleBasedBankGenerator{}.generate({formulaImages});
    if (formulaResult.questions.size() != 1 || !formulaResult.needsReviewQuestions.isEmpty()) return 102;
    if (formulaResult.assets.size() != 1) return 103;
    const QJsonObject formulaQuestion = formulaResult.questions.first().toObject();
    const QJsonObject formulaVisual = formulaQuestion.value("stemImage").toObject();
    const QString formulaVisualPath = formulaVisual.value("path").toString();
    const QImage formulaVisualImage =
        QImage::fromData(formulaResult.assets.value(formulaVisualPath), "PNG");
    if (formulaVisualPath.isEmpty() || formulaVisualImage.isNull() ||
        formulaVisualImage.height() >= 200 ||
        formulaQuestion.value("options").toArray().at(0).toObject().contains("image")) return 104;
    // 每张自动截图都携带原卷定位信息，复核页才能直接回到对应页、以这一框为
    // 初始位置重新裁切；公式行需保留足够高度，不能再把分子分母裁掉。
    if (formulaVisual.value("sourceDocument").toString() != QStringLiteral("formula-options.pdf") ||
        formulaVisual.value("sourcePage").toInt() != 1 ||
        formulaVisual.value("autoCrop").toObject().value("height").toDouble() < 0.08)
        return 108;
    if (!quizpane::validateBank(bankFor(formulaResult), &validationError)) return 105;

    // 有些图形题的 A/B/C/D 是矢量字，根本没有文字层锚点。此时必须截出本题
    // 的完整图阵和四个选项，且下边界止于下一题，不能退化成整张试卷页面。
    ExtractedDocument visualFallback;
    visualFallback.sourcePath = QStringLiteral("visual-fallback.pdf");
    visualFallback.hasPageBoundaries = true;
    visualFallback.plainText = QStringLiteral(
        "86、请从所给的四个选项中，选择最合适的一项填在问号处。\n"
        "87、请从所给的四个选项中，选择最合适的一项填在问号处。\n"
        "答案：A\n");
    visualFallback.questionAnchors.insert(1, {
        {QStringLiteral("86"), QRectF(0.1, 0.28, 0.03, 0.02)},
        {QStringLiteral("87"), QRectF(0.1, 0.70, 0.03, 0.02)}});
    visualFallback.pageImages.insert(1, fillPng);
    const auto visualFallbackResult = RuleBasedBankGenerator{}.generate({visualFallback});
    QJsonObject fallbackQuestion;
    for (const QJsonValue& value : visualFallbackResult.questions) {
        const QJsonObject question = value.toObject();
        if (question.value("stem").toString().contains(QStringLiteral("问号"))) {
            fallbackQuestion = question;
            break;
        }
    }
    if (fallbackQuestion.isEmpty()) return 108;
    const QString fallbackPath = fallbackQuestion.value("stemImage").toObject().value("path").toString();
    const QImage fallbackImage = QImage::fromData(visualFallbackResult.assets.value(fallbackPath), "PNG");
    if (fallbackPath.isEmpty() || fallbackImage.isNull() || fallbackImage.height() >= 500)
        return 109;

    // 图形题可能在页尾只有题干，四个图形选项落到下一页、下一题之前。输出应把
    // 两段局部裁图纵向拼接，不能只截第一页，也不能把下一题题干带进来。
    ExtractedDocument crossPageVisual;
    crossPageVisual.sourcePath = QStringLiteral("cross-page-visual.pdf");
    crossPageVisual.hasPageBoundaries = true;
    crossPageVisual.plainText = QStringLiteral(
        "89、选项中包含 4 个图形，请找出例外。\n正确答案：A\f"
        "90、下一道图形题。\nA、甲\nB、乙\nC、丙\nD、丁\n正确答案：B\n");
    crossPageVisual.questionAnchors.insert(1, {
        {QStringLiteral("89"), QRectF(0.1, 0.70, 0.03, 0.02)}});
    crossPageVisual.questionAnchors.insert(2, {
        {QStringLiteral("90"), QRectF(0.1, 0.35, 0.03, 0.02)}});
    crossPageVisual.pageImages.insert(1, fillPng);
    crossPageVisual.pageImages.insert(2, fillPng);
    const auto crossPageVisualResult = RuleBasedBankGenerator{}.generate({crossPageVisual});
    QJsonObject crossPageVisualQuestion;
    for (const QJsonValue& value : crossPageVisualResult.questions) {
        const QJsonObject question = value.toObject();
        if (question.value("id").toString().contains(QStringLiteral("q89-")))
            crossPageVisualQuestion = question;
    }
    const QString crossPageVisualPath =
        crossPageVisualQuestion.value("stemImage").toObject().value("path").toString();
    const QImage crossPageVisualImage =
        QImage::fromData(crossPageVisualResult.assets.value(crossPageVisualPath), "PNG");
    if (crossPageVisualPath.isEmpty() || crossPageVisualImage.isNull() ||
        crossPageVisualImage.height() < 600)
        return 110;

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
        QStringLiteral("1. 【多选题】下列哪些是容器？\nA. vector\nB. map\nC. mutex\nD. thread\n答案：AB\n");
    const auto multipleResult = RuleBasedBankGenerator{}.generate({multiple});
    if (multipleResult.questions.size() != 1 ||
        !multipleResult.needsReviewQuestions.isEmpty() ||
        multipleResult.questions.first().toObject().value("type").toString() !=
            QStringLiteral("multiple_choice") ||
        multipleResult.questions.first().toObject().value("answer").toObject()
            .value("optionIds").toArray().size() != 2)
        return 8;

    // 异常答案文本即使被提取出多个字母，未标注“多选题”的题也不能被升级成
    // multiple_choice；应转入复核，避免“多选答案至少需要两个 optionId”类误报。
    ExtractedDocument suspiciousSingle;
    suspiciousSingle.sourcePath = QStringLiteral("suspicious-single.txt");
    suspiciousSingle.plainText = QStringLiteral(
        "81. 每年最多开采多少万立方米林木？\nA. 30\nB. 50\nC. 60\nD. 75\n答案：AD\n");
    const auto suspiciousResult = RuleBasedBankGenerator{}.generate({suspiciousSingle});
    if (!suspiciousResult.questions.isEmpty() || suspiciousResult.needsReviewQuestions.size() != 1 ||
        suspiciousResult.needsReviewQuestions.first().toObject().value("type").toString() !=
            QStringLiteral("single_choice") ||
        !suspiciousResult.needsReviewQuestions.first().toObject().value("review").toObject()
             .value("reason").toString().contains(QStringLiteral("未标注多选题")))
        return 108;

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

    // 场景 J：判断题（无 A/B 选项，题干以空括号“（ ）”结尾，答案用对/错/√/×）。
    // 应合成“正确/不正确”两个选项走 true_false 通道，答案归一化到合成选项。
    ExtractedDocument judgment;
    judgment.sourcePath = QStringLiteral("judgment.txt");
    judgment.plainText = QStringLiteral(
        "1. 我国宪法规定公民有受教育的权利和义务。（  ）\n"
        "2. 地球是宇宙的中心。（  ）\n"
        "参考答案\n1.√ 2.×\n");
    const auto judgmentResult = RuleBasedBankGenerator{}.generate({judgment});
    if (judgmentResult.questions.size() != 2 || !judgmentResult.needsReviewQuestions.isEmpty())
        return 44;
    {
        const auto firstQ = judgmentResult.questions.at(0).toObject();
        const auto secondQ = judgmentResult.questions.at(1).toObject();
        if (firstQ.value("type").toString() != QStringLiteral("true_false") ||
            secondQ.value("type").toString() != QStringLiteral("true_false"))
            return 45;
        if (firstQ.value("options").toArray().size() != 2 ||
            firstQ.value("options").toArray().at(0).toObject().value("text").toString() !=
                QStringLiteral("正确") ||
            firstQ.value("options").toArray().at(1).toObject().value("text").toString() !=
                QStringLiteral("不正确"))
            return 46;
        if (firstQ.value("answer").toObject().value("optionIds").toArray().first().toString() !=
                QStringLiteral("a") ||
            secondQ.value("answer").toObject().value("optionIds").toArray().first().toString() !=
                QStringLiteral("b"))
            return 47;
        if (!quizpane::validateBank(bankFor(judgmentResult), &validationError))
            return 48;
    }

    // 场景 K：多选/不定项仅在大标题标注，题干不重复“多选题”。section 信号应向下
    // 传播：多答案题判为 multiple_choice 且不打回复核；不定项单答案题判为 single_choice。
    ExtractedDocument sectionMulti;
    sectionMulti.sourcePath = QStringLiteral("section-multi.txt");
    sectionMulti.plainText = QStringLiteral(
        "一、单项选择题\n"
        "1. 题干一\nA. 甲\nB. 乙\nC. 丙\nD. 丁\n"
        "二、多项选择题\n"
        "2. 下列哪些正确\nA. 甲\nB. 乙\nC. 丙\nD. 丁\n"
        "三、不定项选择题\n"
        "3. 不定项题（答案只有一个时也得分）\nA. 甲\nB. 乙\nC. 丙\nD. 丁\n"
        "参考答案\n1.A 2.ABD 3.A\n");
    const auto sectionMultiResult = RuleBasedBankGenerator{}.generate({sectionMulti});
    if (sectionMultiResult.questions.size() != 3 || !sectionMultiResult.needsReviewQuestions.isEmpty())
        return 49;
    {
        const auto findQ = [&](int number) -> QJsonObject {
            for (const auto& v : sectionMultiResult.questions)
                if (v.toObject().value("id").toString().contains(
                        QStringLiteral("q%1-").arg(number)))
                    return v.toObject();
            return {};
        };
        const QJsonObject q1 = findQ(1);
        const QJsonObject q2 = findQ(2);
        const QJsonObject q3 = findQ(3);
        if (q1.value("type").toString() != QStringLiteral("single_choice") ||
            q2.value("type").toString() != QStringLiteral("multiple_choice") ||
            q3.value("type").toString() != QStringLiteral("single_choice"))
            return 50;
        if (q2.value("answer").toObject().value("optionIds").toArray().size() != 3)
            return 51;
        if (!quizpane::validateBank(bankFor(sectionMultiResult), &validationError))
            return 52;
    }

    // 场景 L：真题可能只用“（多选题）”标整段；题干中的（1）…（4）是条件，
    // 后续另有 A/B/C/D，不能抢占选项；答案解析开头的“2、A 项错误”也不能
    // 覆盖结尾明确写出的“故正确答案为 C”。
    ExtractedDocument beijingRegression;
    beijingRegression.sourcePath = QStringLiteral("beijing-regression.txt");
    beijingRegression.plainText = QStringLiteral(
        "（多选题）\n"
        "根据下列统计资料回答问题。\n"
        "434.5 亿元，增长 49%。\n2096.4 亿元，增长 2.8%。\n"
        "1. 下列哪些正确\nA. 甲\nB. 乙\nC. 丙\nD. 丁\n"
        "二、单项选择题\n"
        "2. 根据条件选择答案\n（1）条件甲\n（2）条件乙\n（3）条件丙\n（4）条件丁\n"
        "A. 真选项甲\nB. 真选项乙\nC. 真选项丙\nD. 真选项丁\n"
        "答案及解析\n"
        "1、A 项正确，其余略。\n故正确答案为 B、D。\n"
        "2、A 项错误，其余略。\n434.5 亿元只是计算中间值。\n故正确答案为 C。\n");
    const auto beijingResult = RuleBasedBankGenerator{}.generate({beijingRegression});
    if (beijingResult.questions.size() != 2 || !beijingResult.needsReviewQuestions.isEmpty())
        return 53;
    {
        const QJsonObject multi = beijingResult.questions.at(0).toObject();
        const QJsonObject conditioned = beijingResult.questions.at(1).toObject();
        if (multi.value("type").toString() != QStringLiteral("multiple_choice") ||
            multi.value("answer").toObject().value("optionIds").toArray().size() != 2)
            return 54;
        if (conditioned.value("answer").toObject().value("optionIds").toArray().first().toString() !=
            QStringLiteral("c"))
            return 55;
        const QJsonArray conditionedOptions = conditioned.value("options").toArray();
        if (conditionedOptions.size() != 4 ||
            conditionedOptions.at(0).toObject().value("text").toString() !=
                QStringLiteral("真选项甲") ||
            !conditioned.value("stem").toString().contains(QStringLiteral("（4）条件丁")))
            return 56;
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

    // 批量统计审计：同一份文档里 5 道题，第 3 题缺一个选项（3 个 vs 众数 4 个）、
    // 题号缺第 4 题。样本量 ≥4 才启用众数/中位数统计；答案分布检测要求 ≥8 题
    // 才启用（避免小题库正常撞车触发误报），这里 5 题不会命中该项。同时验证
    // 未命中信号的题目（第 2 题）不会被误伤。
    ExtractedDocument auditDocument;
    auditDocument.sourcePath = QStringLiteral("audit-batch.txt");
    auditDocument.plainText = QStringLiteral(
        "1. 第一题？\nA. 甲\nB. 乙\nC. 丙\nD. 丁\n答案：A\n"
        "2. 第二题？\nA. 甲\nB. 乙\nC. 丙\nD. 丁\n答案：B\n"
        "3. 第三题？\nA. 甲\nB. 乙\nC. 丙\n答案：A\n"
        "5. 第五题？\nA. 甲\nB. 乙\nC. 丙\nD. 丁\n答案：A\n"
        "6. 第六题？\nA. 甲\nB. 乙\nC. 丙\nD. 丁\n答案：A\n");
    const auto auditResult = RuleBasedBankGenerator{}.generate({auditDocument});
    if (auditResult.questions.size() != 5 || !auditResult.needsReviewQuestions.isEmpty())
        return 114;
    bool sawMissingNumberWarning = false;
    for (const QString& warning : auditResult.warnings)
        if (warning.contains(QStringLiteral("第 4 题"))) sawMissingNumberWarning = true;
    if (!sawMissingNumberWarning)
        return 115;
    QJsonObject auditQuestion3, auditQuestion2;
    for (const auto& value : auditResult.questions) {
        const QJsonObject question = value.toObject();
        if (question.value("id").toString().contains(QStringLiteral("-q3-")))
            auditQuestion3 = question;
        if (question.value("id").toString().contains(QStringLiteral("-q2-")))
            auditQuestion2 = question;
    }
    if (auditQuestion3.isEmpty() || auditQuestion2.isEmpty())
        return 116;
    const QJsonObject question3Review = auditQuestion3.value("review").toObject();
    if (!question3Review.value("needsReview").toBool() ||
        question3Review.value("riskLevel").toString() != QStringLiteral("soft") ||
        !question3Review.value("signals").toArray().contains(QStringLiteral("option-count-outlier")))
        return 117;
    // 第 2 题答案是 B（非多数派），不应被答案分布信号误伤；也不应携带
    // option-count-outlier（选项数与众数一致）。
    const QJsonObject question2Review = auditQuestion2.value("review").toObject();
    if (question2Review.value("needsReview").toBool())
        return 118;

    return 0;
}
