#include "quizpane/studio/rule_based_generator.hpp"
#include "quizpane/studio/option_label.hpp"

#include <QFileInfo>
#include <QBuffer>
#include <QCryptographicHash>
#include <QHash>
#include <QMap>
#include <QImage>
#include <QJsonObject>
#include <QPainter>
#include <QRegularExpression>
#include <QSet>
#include <QtMath>

#include <limits>
#include <algorithm>

namespace quizpane::studio {
namespace {

struct SourceLine {
    QString text;
    int page = 0;
    // OCR 会保留真正的段间空行；不能在 sourceLines 阶段丢掉它，否则后面无法
    // 区分“视觉换行”和“自然段换行”。普通题目解析继续忽略该标志，长材料/题干
    // 则可据它恢复段落。
    bool paragraphBreakBefore = false;
};

struct QuestionAnchor {
    int line = 0;
    int number = 0;
    QString firstStemLine;
    bool inferredNumber = false;
};

struct MaterialMarker {
    int line = 0;
    int contentLine = 0;
    int firstQuestionLine = -1;
    int firstNumber = 0;
    int lastNumber = 0;
    QString id;
};

qint64 assetBytes(const QHash<QString, QByteArray>& assets) {
    qint64 total = 0;
    for (auto it = assets.cbegin(); it != assets.cend(); ++it)
        total += it.value().size();
    return total;
}

const QRegularExpression& questionPattern() {
    // `\.(?!\d)` 用来把 “1.5 倍”这类小数挡在题号之外。但资料分析题的题干几乎
    // 都以年份开头（“111.2016年～2020年，……”），负向前瞻会连真题号一起否掉，
    // 导致整份卷子只剩题干不以数字开头的那几道题。
    // 追加一条例外：句点后是四位数字且紧跟年份记号（“年”或区间连接符）时按题号
    // 处理——真小数不会写成 “.2016年”，因而不会被这条例外误伤。
    static const QRegularExpression value(QStringLiteral(
        R"(^\s*(?:(?:问题|题目)\s*)?(?:第\s*)?(\d{1,4})\s*(?:题|[．、:：\)）]|\.(?=\d{4}\s*[年\-~～—]|\d{1,2}\s*世纪)|\.(?!\d))\s*(.*)$)"));
    return value;
}

void normalizeTrailingQuestionNumberLayout(QList<SourceLine>* lines) {
    if (!lines)
        return;
    // 一些网页导出的 PDF 把题号绘制在题干之后（“题干…\n1.\nA.…”），而不是
    // 常见的“1. 题干…”。文字层会忠实保留这个视觉顺序，原来的锚点虽能看到
    // 题号，却把题干留在上一题块外，最终所有题都不完整。仅在题号行没有任何
    // 题干文字时，把紧邻的前置题干折回题号行；选项行、答案提示和上一题号都是
    // 硬边界，绝不跨题拼接。
    static const QRegularExpression resultBoundary(
        QStringLiteral(R"(^\s*(?:正确答案|你的答案)\s*[:：])"));
    static const QRegularExpression optionRow(
        QStringLiteral(R"((?:^|\s)A\s*[\.．、:：\)）].*(?:\s)B\s*[\.．、:：\)）])"));
    static const QRegularExpression optionLine(
        QStringLiteral(R"(^\s*(?:[A-Fa-f]\s*[.．、:：)）]|[①②③④⑤⑥⑦⑧⑨⑩]))"));
    for (int index = 0; index < lines->size(); ++index) {
        const auto marker = questionPattern().match(lines->at(index).text);
        if (!marker.hasMatch() || !marker.captured(2).trimmed().isEmpty())
            continue;
        int start = index;
        while (start > 0) {
            const QString previous = lines->at(start - 1).text;
            const auto previousMarker = questionPattern().match(previous);
            if (previous.trimmed().isEmpty() || resultBoundary.match(previous).hasMatch() ||
                optionRow.match(previous).hasMatch() || optionLine.match(previous).hasMatch() ||
                previousMarker.hasMatch())
                break;
            --start;
        }
        if (start == index)
            continue;
        SourceLine& firstStemLine = (*lines)[start];
        firstStemLine.text = QStringLiteral("%1. %2")
            .arg(marker.captured(1), firstStemLine.text.trimmed());
        // 将题号锚点回贴到题干首行的页码，后续 source 与视觉裁切仍指向题干。
        lines->removeAt(index);
        index = start;
    }
}

// 极少数原卷本身就漏印题号（例如 10 题后正文直接以“20世纪以来”开头，下一题
// 却是 12）。只在 n 与 n+2 之间同时满足以下证据时补 n+1：上一题已有 D 选项、
// 候选正文与 D 选项存在明显视觉段距、候选块在下一题前拥有完整 A-D。补号题仍
// 强制进入复核，避免把普通续段猜成题目。
QSet<int> recoverSingleMissingQuestionNumbers(ExtractedDocument* document,
                                               QList<SourceLine>* lines) {
    QSet<int> recoveredLines;
    if (!document || !lines || lines->isEmpty())
        return recoveredLines;
    struct ExistingAnchor { int line; int number; };
    QList<ExistingAnchor> existing;
    for (int index = 0; index < lines->size(); ++index) {
        const auto match = questionPattern().match(lines->at(index).text);
        if (match.hasMatch() && match.captured(1).toInt() > 0)
            existing.append({index, match.captured(1).toInt()});
    }
    static const QRegularExpression optionAtStart(
        QStringLiteral(R"(^\s*([A-Da-d])\s*[.．、:：)）])"));
    static const QRegularExpression optionAnywhere(
        QStringLiteral(R"((?<![A-Za-z0-9])([A-Da-d])\s*[.．、:：)）])"));
    static const QRegularExpression forbiddenCandidate(QStringLiteral(
        R"(^\s*(?:答案|参考答案|正确答案|解析|第\s*[一二三四五六七八九十\d]+\s*(?:部分|章|节)))"));
    const auto boundsFor = [document](const SourceLine& line) {
        for (const PdfTextAnchor& anchor : document->lineAnchors.value(line.page))
            if (anchor.text.simplified() == line.text.simplified())
                return anchor.bounds;
        return QRectF{};
    };
    for (int pair = 0; pair + 1 < existing.size(); ++pair) {
        const ExistingAnchor previous = existing.at(pair);
        const ExistingAnchor next = existing.at(pair + 1);
        if (next.number != previous.number + 2 || next.line <= previous.line + 2)
            continue;
        int previousOptionD = -1;
        for (int index = previous.line + 1; index < next.line; ++index) {
            const auto option = optionAtStart.match(lines->at(index).text);
            if (option.hasMatch() && option.captured(1).compare(QStringLiteral("d"),
                                                                Qt::CaseInsensitive) == 0) {
                previousOptionD = index;
                break;
            }
        }
        const int candidate = previousOptionD + 1;
        if (previousOptionD < 0 || candidate >= next.line ||
            lines->at(candidate).text.size() < 12 ||
            questionPattern().match(lines->at(candidate).text).hasMatch() ||
            optionAtStart.match(lines->at(candidate).text).hasMatch() ||
            forbiddenCandidate.match(lines->at(candidate).text).hasMatch())
            continue;

        QSet<QString> candidateOptions;
        for (int index = candidate + 1; index < next.line; ++index) {
            auto matches = optionAnywhere.globalMatch(lines->at(index).text);
            while (matches.hasNext())
                candidateOptions.insert(matches.next().captured(1).toLower());
        }
        if (!candidateOptions.contains(QStringLiteral("a")) ||
            !candidateOptions.contains(QStringLiteral("b")) ||
            !candidateOptions.contains(QStringLiteral("c")) ||
            !candidateOptions.contains(QStringLiteral("d")))
            continue;

        const QRectF optionBounds = boundsFor(lines->at(previousOptionD));
        const QRectF candidateBounds = boundsFor(lines->at(candidate));
        if (optionBounds.isEmpty() || candidateBounds.isEmpty() ||
            lines->at(previousOptionD).page != lines->at(candidate).page ||
            candidateBounds.top() - optionBounds.bottom() <=
                qMax<qreal>(0.004, qMax(optionBounds.height(), candidateBounds.height()) * 0.35))
            continue;

        const QString originalText = lines->at(candidate).text;
        lines->operator[](candidate).text =
            QStringLiteral("%1. %2").arg(previous.number + 1).arg(originalText);
        document->questionAnchors[lines->at(candidate).page].append(
            {QString::number(previous.number + 1), candidateBounds});
        document->warnings.append(
            QStringLiteral("第 %1 页有一处原卷缺失题号，已根据前后题号与完整选项补为第 %2 题，需人工核对")
                .arg(lines->at(candidate).page).arg(previous.number + 1));
        recoveredLines.insert(candidate);
    }
    return recoveredLines;
}

const QRegularExpression& inlineAnswerPattern() {
    static const QRegularExpression value(QStringLiteral(
        R"(^\s*(?:【?\s*(?:(?:参考|标准)?答案)\s*】?|正确答案)\s*[:：]?\s*(.+?)\s*$)"));
    return value;
}

const QRegularExpression& solutionPattern() {
    static const QRegularExpression value(
        QStringLiteral(R"(^\s*(?:【\s*(?:(?:答案)?解析|解答|说明)\s*】\s*[:：]?\s*|(?:(?:答案)?解析|解答|说明)(?:\s*[:：]\s*|\s+|$))(.*)$)"));
    return value;
}

bool isAnswerSectionHeader(const QString& text) {
    // “答案对照表”与“参考答案”同为整卷答案区的固定标题（常见于按题号区间
    // 分组的答案页），必须识别，否则区间答案会被当成普通正文漏掉。
    static const QRegularExpression pattern(
        QStringLiteral(R"(^\s*【?\s*(?:答案|参考答案|答案对照表|答案汇总|答案及解析|参考答案及解析)\s*】?\s*[:：]?\s*$)"));
    return pattern.match(text).hasMatch();
}

// 答案串与标题排在同一行的答案区头，例如“【参考答案】CAACD DBABC”。
// 排版紧凑的真题（以及按视觉行还原文本的解析后端）常把二者合并，此时若坚持
// 要求标题独占一行，整份答案都会被漏掉、每道题都以“未识别到答案”打回复核。
//
// 判定必须比“标题后有内容”更严：单题的“答案：A”同样是标题加内容，把它当成
// 答案区起点会切断题目块，反而让本来正常的题失去答案。区分依据是措辞——
// “参考答案/答案汇总”是整卷答案页的固定说法，两个字母起就足以判定；裸“答案”
// 则同时用于单题，要求长到不可能是单题多选（六个字母以上）才算答案区。
bool isInlineAnswerSectionHeader(const QString& text) {
    static const QRegularExpression collective(QStringLiteral(
        R"(^\s*【?\s*(?:参考答案|答案汇总|答案及解析|参考答案及解析)\s*】?\s*[:：]?\s*)"
        R"((?=(?:[A-Fa-f]\s*){2,}$)[A-Fa-f\s]+$)"));
    static const QRegularExpression bare(QStringLiteral(
        R"(^\s*【?\s*答案\s*】?\s*[:：]?\s*(?=(?:[A-Fa-f]\s*){6,}$)[A-Fa-f\s]+$)"));
    return collective.match(text).hasMatch() || bare.match(text).hasMatch();
}

bool isMaterialHeader(const QString& text) {
    static const QRegularExpression pattern(QStringLiteral(
        R"(^\s*(?:[（(][一二三四五六七八九十\d]+[）)]\s*)?(?:(?:材料|资料|阅读材料)\s*[一二三四五六七八九十\d]*(?:\s*[:：].*|\s*)|阅读(?:下列|以下)(?:材料|文字).*|根据(?:下列|以下)(?:统计)?(?:资料|材料).*|原文\s*[:：].*)$)"));
    return pattern.match(text).hasMatch();
}

bool isMaterialSectionMarker(const QString& text) {
    static const QRegularExpression pattern(
        QStringLiteral(R"(^\s*[（(][一二三四五六七八九十\d]+[）)]\s*$)"));
    return pattern.match(text).hasMatch();
}

bool isTopLevelSectionHeading(const QString& text) {
    static const QRegularExpression pattern(
        QStringLiteral(R"(^\s*[一二三四五六七八九十]+、\s*\S.{1,120}$)"));
    return pattern.match(text).hasMatch();
}

bool materialQuestionMentionsUnderline(const QList<SourceLine>& lines,
                                       const QList<QuestionAnchor>& anchors,
                                       const MaterialMarker& material,
                                       int materialEndLine) {
    static const QRegularExpression cue(
        QStringLiteral(R"(划线|横线|标注|标记)"));
    for (int anchorIndex = 0; anchorIndex < anchors.size(); ++anchorIndex) {
        const QuestionAnchor& anchor = anchors.at(anchorIndex);
        if (anchor.line < material.firstQuestionLine || anchor.line >= materialEndLine)
            continue;
        if (material.firstNumber > 0 && material.lastNumber > 0 &&
            (anchor.number < material.firstNumber || anchor.number > material.lastNumber))
            continue;
        const int nextAnchorLine = anchorIndex + 1 < anchors.size()
            ? qMin(materialEndLine, anchors.at(anchorIndex + 1).line)
            : materialEndLine;
        // 题干偶尔会在 PDF 中折为两行；因此从题号扫到下一题号，而不是只看
        // questionPattern 捕获到的第一行。命中只决定是否检查该共享材料，并不
        // 直接猜测哪一个词有下划线。
        for (int line = anchor.line; line < nextAnchorLine; ++line)
            if (cue.match(lines.at(line).text).hasMatch())
                return true;
    }
    return false;
}

bool isDataAnalysisPartHeading(const QString& text) {
    static const QRegularExpression pattern(QStringLiteral(
        R"(^\s*(?:第\s*[一二三四五六七八九十\d]+\s*部分|[一二三四五六七八九十]+、)\s*资料分析(?:\s*[，,].*)?\s*$)"));
    return pattern.match(text).hasMatch();
}

bool isGraphicalReasoningPartHeading(const QString& text) {
    static const QRegularExpression pattern(QStringLiteral(
        R"(^\s*(?:第\s*[一二三四五六七八九十\d]+\s*部分|[一二三四五六七八九十]+、)\s*图形推理(?:\s*[，,].*)?\s*$)"));
    return pattern.match(text).hasMatch();
}

bool isInsideDataAnalysisPart(const QList<SourceLine>& lines, int line) {
    static const QRegularExpression partHeading(
        QStringLiteral(R"(^\s*第\s*[一二三四五六七八九十\d]+\s*部分.*$)"));
    for (int cursor = line; cursor >= 0; --cursor) {
        const QString text = lines.at(cursor).text.trimmed();
        if (isDataAnalysisPartHeading(text))
            return true;
        if (partHeading.match(text).hasMatch())
            return false;
    }
    return false;
}

// 判定题目所在大标题是否为“图形推理”整题型分区，用于高危复核信号标注：图形推理
// 靠像素图判断规律，规则引擎无法验证正确性，即使结构完整也必须提示复核。
bool isInsideGraphicalReasoningPart(const QList<SourceLine>& lines, int line) {
    static const QRegularExpression partHeading(
        QStringLiteral(R"(^\s*第\s*[一二三四五六七八九十\d]+\s*部分.*$)"));
    for (int cursor = line; cursor >= 0; --cursor) {
        const QString text = lines.at(cursor).text.trimmed();
        if (isGraphicalReasoningPartHeading(text))
            return true;
        if (partHeading.match(text).hasMatch())
            return false;
    }
    return false;
}

// 多答案题型大标题：真题常在 section 标题里标一次题型，下面每道题干不再重复。
// 匹配“二、多项选择题 / 不定项选择题 / 多选 / 多项选择”等。命中即表示该
// 段题目允许多个正确答案，向下传播给段内每道题，避免“未标注多选却多答案”
// 把整段多选/不定项打回复核。不定项与多选在作答结构上等价（≥2 选项、≥1 答案），
// 统一映射为 multiple_choice，不新增题型，兼容现有 schema/校验器/前端。
bool isMultiAnswerSectionHeading(const QString& text) {
    static const QRegularExpression pattern(
        QStringLiteral(R"(多选|多项选|不定项|多项选择|多项选择题)"));
    if (!pattern.match(text).hasMatch())
        return false;
    static const QRegularExpression parenthesized(
        QStringLiteral(R"(^\s*[（(][^）)]*(?:多选|多项选|不定项)[^）)]*[）)]\s*$)"));
    return isTopLevelSectionHeading(text) || parenthesized.match(text).hasMatch();
}

QString assetBaseName(const QString& sourcePath) {
    QString base = QFileInfo(sourcePath).completeBaseName().toLower();
    base.replace(QRegularExpression(QStringLiteral("[^a-z0-9._-]+")), QStringLiteral("-"));
    base.remove(QRegularExpression(QStringLiteral("^-+|-+$")));
    if (base.isEmpty()) base = QStringLiteral("source");
    const QString fingerprint = QString::fromLatin1(
        QCryptographicHash::hash(sourcePath.toUtf8(), QCryptographicHash::Sha1).toHex().left(10));
    return base.left(40) + QChar('-') + fingerprint;
}

QJsonObject visualAssetDescriptor(const ExtractedDocument& document, int page,
                                  const QRectF& normalizedCrop, const QString& path,
                                  const QString& alt) {
    // 附件同时保留原卷页与自动裁切框。它们只用于制作器的临时复核：用户可以
    // 一键回到同一页、以规则给出的题目范围为初始框重新裁切，而不是从 PDF 首页
    // 开始寻找。来源仅记录文件名、页码和归一化坐标，不包含本机绝对路径；不认识
    // 这些可选字段的旧版题库消费者仍可继续只读取 path/alt。
    return {{QStringLiteral("path"), path},
            {QStringLiteral("alt"), alt},
            {QStringLiteral("sourceDocument"), QFileInfo(document.sourcePath).fileName()},
            {QStringLiteral("sourcePage"), page},
            {QStringLiteral("autoCrop"), QJsonObject{
                {QStringLiteral("x"), normalizedCrop.x()},
                {QStringLiteral("y"), normalizedCrop.y()},
                {QStringLiteral("width"), normalizedCrop.width()},
                {QStringLiteral("height"), normalizedCrop.height()}}}};
}

QRectF questionBoundsFor(const ExtractedDocument& document, int page, int number) {
    const auto& anchors = document.questionAnchors.value(page);
    QRectF result;
    int matches = 0;
    for (const PdfTextAnchor& anchor : anchors)
        if (anchor.text.toInt() == number) { result = anchor.bounds; ++matches; }
    // 同页重号不能任取第一个坐标，否则两题会共用错误的裁图。
    return matches == 1 ? result : QRectF{};
}

QRectF lineBoundsFor(const ExtractedDocument& document, int page, const QString& text) {
    const QString wanted = text.trimmed();
    for (const PdfTextAnchor& anchor : document.lineAnchors.value(page))
        if (anchor.text == wanted)
            return anchor.bounds;
    return {};
}

QRectF stemBoundsFor(const ExtractedDocument& document, int page,
                     const QStringList& stemSourceLines) {
    QRectF result;
    for (const QString& sourceLine : stemSourceLines) {
        QRectF bounds = lineBoundsFor(document, page, sourceLine);
        // 网页导出 PDF 的文字对象偶尔把裸题号排在题干之后。归一化后源码行会
        // 补上“1. ”，但 PDF 的原始行锚点仍只有题干本身；去掉题号后再找一次，
        // 才能以题干最后一行而不是题号位置作为插图裁切起点。
        if (bounds.isEmpty()) {
            const auto marker = questionPattern().match(sourceLine);
            if (marker.hasMatch() && !marker.captured(2).trimmed().isEmpty())
                bounds = lineBoundsFor(document, page, marker.captured(2));
        }
        if (!bounds.isEmpty())
            result = result.isEmpty() ? bounds : result.united(bounds);
    }
    return result;
}

QString restoreDroppedBlankLines(QString value) {
    // 【甲】/【乙】/【丙】是原题用来指代三个填词位置的编号，必须原样保留；
    // 它们不是待输入框。只有 PDF 文字层丢掉的空白横线才补成可渲染的占位。
    // 很多 PDF 把空白横线作为单独的矢量/图片对象，文字层只留下“的 。”。
    // 仅在汉字与紧随的中文标点之间补占位，避免把正常的段落空格误判成填空。
    static const QRegularExpression droppedUnderline(
        QStringLiteral(R"((\p{Han})[ \t]+([，。；、]))"));
    value.replace(droppedUnderline, QStringLiteral("\\1〔填空〕\\2"));
    // 有些 PDF 的下划线能被提取成 ASCII/全角下划线而不是绘图对象；统一成题库
    // 可保存的占位符，复核页和作答端再将它渲染为真正的横线。
    value.replace(QRegularExpression(QStringLiteral("(?:_{2,}|＿{2,})")),
                  QStringLiteral("〔填空〕"));
    return value;
}

QJsonArray extractMaterialLayoutImages(const ExtractedDocument& document, int firstPage,
                                       int lastPage, const QString& firstLine,
                                       const QString& firstQuestionLine,
                                       const QString& materialId,
                                       bool chartOnly,
                                       QHash<QString, QByteArray>* assets) {
    QJsonArray images;
    if (firstPage <= 0 || lastPage < firstPage)
        return images;
    for (int page = firstPage; page <= lastPage; ++page) {
        const QImage source = QImage::fromData(document.pageImages.value(page), "PNG");
        if (source.isNull())
            continue;
        qreal top = page == firstPage
            ? qMax<qreal>(0.0, lineBoundsFor(document, page, firstLine).top() - 0.012)
            : 0.015;
        qreal bottom = 0.985;
        if (page == lastPage) {
            // 以完整题干首行作为终点，不能只依赖题号锚点：有些卷子题号形式为
            // “44、第…”，QPdf 的数字选择范围会失效。找不到完整题干锚点时宁可
            // 不产出本页截图，也绝不能把题目裁进资料图。
            const QRectF questionBounds = lineBoundsFor(document, page, firstQuestionLine);
            if (questionBounds.isEmpty())
                continue;
            bottom = qMin<qreal>(bottom, questionBounds.top() - 0.012);
        }
        if (bottom <= top + 0.02)
            continue;
        const QRectF normalizedCrop(0.04, top, 0.92, bottom - top);
        const QRect crop(qFloor(source.width() * normalizedCrop.x()),
                         qFloor(source.height() * normalizedCrop.y()),
                         qCeil(source.width() * normalizedCrop.width()),
                         qCeil(source.height() * normalizedCrop.height()));
        const QImage snippet = source.copy(crop.intersected(source.rect()));
        if (chartOnly) {
            // 资料分析正文已经以结构化文本保存；这里只保留真正的统计图或表格。
            // 表格边框、坐标轴和柱形边缘都会形成明显的长直线，而普通段落文字
            // 不会。先铺白底，避免 PDF 页面的透明空白被误判成整幅黑色长线。
            QImage flat(snippet.size(), QImage::Format_RGB32);
            flat.fill(Qt::white);
            QPainter painter(&flat);
            painter.drawImage(0, 0, snippet);
            painter.end();
            const QImage gray = flat.convertToFormat(QImage::Format_Grayscale8);
            const auto hasLongRun = [&](bool horizontal, int required) {
                const int outer = horizontal ? gray.height() : gray.width();
                const int inner = horizontal ? gray.width() : gray.height();
                for (int a = 0; a < outer; ++a) {
                    int run = 0;
                    int gaps = 0;
                    for (int b = 0; b < inner; ++b) {
                        const int x = horizontal ? b : a;
                        const int y = horizontal ? a : b;
                        const bool dark = gray.constScanLine(y)[x] < 175;
                        if (dark) {
                            ++run;
                            gaps = 0;
                        } else if (run > 0 && gaps < 2) {
                            ++run;
                            ++gaps;
                        } else {
                            if (run >= required) return true;
                            run = 0;
                            gaps = 0;
                        }
                    }
                    if (run >= required) return true;
                }
                return false;
            };
            const bool hasChartGeometry =
                hasLongRun(true, qMax(40, gray.width() / 5)) ||
                hasLongRun(false, qMax(40, gray.height() / 6));
            if (!hasChartGeometry)
                continue;
        }
        QByteArray png;
        QBuffer buffer(&png);
        if (snippet.isNull() || !buffer.open(QIODevice::WriteOnly) || !snippet.save(&buffer, "PNG"))
            continue;
        const QString path = QStringLiteral("assets/%1-%2-p%3.png")
            .arg(assetBaseName(document.sourcePath), materialId).arg(page);
        assets->insert(path, png);
        images.append(visualAssetDescriptor(document, page, normalizedCrop, path, chartOnly
            ? QStringLiteral("原卷资料图表")
            : QStringLiteral("原卷材料版式（含下划线和填空）")));
    }
    return images;
}

QJsonObject captureSourceBlock(ExtractedDocument* document, const QList<SourceLine>& lines,
                               int start, int end, const QString& id,
                               QHash<QString, QByteArray>* assets) {
    QMap<int, QRectF> regions;
    for (int i = start; i < end; ++i) {
        const auto& line = lines.at(i);
        const QRectF bounds = lineBoundsFor(*document, line.page, line.text);
        if (line.page > 0 && !bounds.isEmpty()) regions[line.page] = regions.value(line.page).united(bounds);
    }
    if (regions.isEmpty() || regions.size() > 4) return {};
    ensurePdfPageImages(document, regions.keys());
    QList<QImage> pieces;
    int width = 0, height = 0;
    QRectF firstCrop;
    for (auto it = regions.cbegin(); it != regions.cend(); ++it) {
        const QImage page = QImage::fromData(document->pageImages.value(it.key()), "PNG");
        if (page.isNull()) return {}; // 不把缺页的截图当成完整原文。
        const qreal top = qMax<qreal>(0, it.value().top() - 0.004);
        const qreal bottom = qMin<qreal>(1, it.value().bottom() + 0.004);
        const QRectF crop(0.04, top, 0.92, bottom - top);
        if (firstCrop.isEmpty()) firstCrop = crop;
        const QImage piece = page.copy(QRect(qFloor(crop.x() * page.width()), qFloor(crop.y() * page.height()),
            qCeil(crop.width() * page.width()), qCeil(crop.height() * page.height())).intersected(page.rect()));
        if (piece.isNull()) return {};
        pieces.append(piece);
        width = qMax(width, piece.width());
        height += piece.height();
    }
    QImage combined(width, height, QImage::Format_RGB32);
    combined.fill(Qt::white);
    QPainter painter(&combined);
    int y = 0;
    for (const auto& piece : pieces) { painter.drawImage(0, y, piece); y += piece.height(); }
    painter.end();
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly) || !combined.save(&buffer, "PNG")) return {};
    const QString path = QStringLiteral("assets/%1-%2-reference.png").arg(assetBaseName(document->sourcePath), id);
    assets->insert(path, bytes);
    return visualAssetDescriptor(*document, regions.firstKey(), firstCrop, path, QStringLiteral("原卷题目（请核对版式或题目边界）"));
}

QJsonObject describeLazySourceBlock(const ExtractedDocument& document,
                                    const QList<SourceLine>& lines, int start, int end,
                                    const QString& id) {
    QMap<int, QRectF> regions;
    for (int i = start; i < end; ++i) {
        const auto& line = lines.at(i);
        const QRectF bounds = lineBoundsFor(document, line.page, line.text);
        if (line.page > 0 && !bounds.isEmpty())
            regions[line.page] = regions.value(line.page).united(bounds);
    }
    if (regions.isEmpty() || regions.size() > 4)
        return {};
    QJsonArray segments;
    QRectF firstCrop;
    for (auto it = regions.cbegin(); it != regions.cend(); ++it) {
        const qreal top = qMax<qreal>(0, it.value().top() - 0.004);
        const qreal bottom = qMin<qreal>(1, it.value().bottom() + 0.004);
        const QRectF crop(0.04, top, 0.92, bottom - top);
        if (crop.isEmpty())
            continue;
        if (firstCrop.isEmpty())
            firstCrop = crop;
        segments.append(QJsonObject{
            {QStringLiteral("sourcePage"), it.key()},
            {QStringLiteral("crop"), QJsonObject{
                {QStringLiteral("x"), crop.x()}, {QStringLiteral("y"), crop.y()},
                {QStringLiteral("width"), crop.width()},
                {QStringLiteral("height"), crop.height()}}}});
    }
    if (segments.isEmpty())
        return {};
    const QString path = QStringLiteral("assets/%1-%2-reference.png")
        .arg(assetBaseName(document.sourcePath), id);
    QJsonObject descriptor = visualAssetDescriptor(
        document, regions.firstKey(), firstCrop, path,
        QStringLiteral("原卷题目（请核对版式或题目边界）"));
    descriptor.insert(QStringLiteral("lazyReview"), true);
    descriptor.insert(QStringLiteral("reviewSegments"), segments);
    return descriptor;
}

QJsonObject describeLazySourcePage(const ExtractedDocument& document, int page,
                                   const QString& id) {
    if (page <= 0)
        return {};
    const QString path = QStringLiteral("assets/%1-%2-reference.png")
        .arg(assetBaseName(document.sourcePath), id);
    QJsonObject descriptor = visualAssetDescriptor(
        document, page, QRectF(0.0, 0.0, 1.0, 1.0), path,
        QStringLiteral("原卷整页（请手动框选题目区域）"));
    descriptor.insert(QStringLiteral("lazyReview"), true);
    descriptor.insert(QStringLiteral("reviewSegments"), QJsonArray{QJsonObject{
        {QStringLiteral("sourcePage"), page},
        {QStringLiteral("crop"), QJsonObject{
            {QStringLiteral("x"), 0.0}, {QStringLiteral("y"), 0.0},
            {QStringLiteral("width"), 1.0}, {QStringLiteral("height"), 1.0}}}}});
    return descriptor;
}

QHash<QString, QRectF> optionRowForQuestion(const ExtractedDocument& document, int page,
                                             int number) {
    const QRectF questionBounds = questionBoundsFor(document, page, number);
    if (questionBounds.isEmpty())
        return {};
    // A/B/C/D 可能被 PDF 文字层提取到下一题的选项行。它们虽然同样位于当前
    // 题号之后，却绝不能拿来裁当前题的图片；例如第 76 题会把第 77 题的文字
    // 和题号混进四个分数选项。先把候选行限制在本题与下一题题号之间。
    qreal nextQuestionTop = 1.0;
    for (const PdfTextAnchor& anchor : document.questionAnchors.value(page)) {
        if (anchor.bounds.top() > questionBounds.bottom())
            nextQuestionTop = qMin(nextQuestionTop, anchor.bounds.top());
    }
    const auto labels = document.optionLabelAnchors.value(page);
    QHash<QString, QRectF> best;
    qreal bestY = std::numeric_limits<qreal>::max();
    for (const PdfTextAnchor& candidate : labels) {
        if (candidate.bounds.top() <= questionBounds.bottom() ||
            candidate.bounds.top() >= nextQuestionTop)
            continue;
        // 一行 A/B/C/D 的标签应当在近似相同的 y 位置。先以每个 A 为候选行，
        // 选择题号之后最靠上的完整一行，避免拿到本页下一题的选项标签。
        if (candidate.text != QStringLiteral("a"))
            continue;
        QHash<QString, QRectF> row{{QStringLiteral("a"), candidate.bounds}};
        for (const PdfTextAnchor& other : labels) {
            if (other.bounds.top() <= questionBounds.bottom() ||
                other.bounds.top() >= nextQuestionTop ||
                qAbs(other.bounds.center().y() - candidate.bounds.center().y()) > 0.018)
                continue;
            if (QStringLiteral("abcd").contains(other.text) && !row.contains(other.text))
                row.insert(other.text, other.bounds);
        }
        if (row.size() == 4 && candidate.bounds.top() < bestY) {
            best = row;
            bestY = candidate.bounds.top();
        }
    }
    return best;
}

qreal firstOptionLabelTopForQuestion(const ExtractedDocument& document, int page, int number) {
    const QRectF questionBounds = questionBoundsFor(document, page, number);
    if (questionBounds.isEmpty())
        return std::numeric_limits<qreal>::max();
    qreal nextQuestionTop = 1.0;
    for (const PdfTextAnchor& anchor : document.questionAnchors.value(page)) {
        if (anchor.bounds.top() > questionBounds.bottom())
            nextQuestionTop = qMin(nextQuestionTop, anchor.bounds.top());
    }
    qreal first = std::numeric_limits<qreal>::max();
    for (const PdfTextAnchor& label : document.optionLabelAnchors.value(page)) {
        if (label.bounds.top() <= questionBounds.bottom() ||
            label.bounds.top() >= nextQuestionTop)
            continue;
        if (QStringLiteral("abcd").contains(label.text, Qt::CaseInsensitive))
            first = qMin(first, label.bounds.top());
    }
    return first;
}

QHash<QString, QJsonObject> extractOptionImages(const ExtractedDocument& document, int page,
                                                 int number, QHash<QString, QByteArray>* assets) {
    const QHash<QString, QRectF> labels = optionRowForQuestion(document, page, number);
    if (labels.size() != 4 || !document.pageImages.contains(page))
        return {};
    QImage source = QImage::fromData(document.pageImages.value(page), "PNG");
    if (source.isNull())
        return {};
    const QStringList ids{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"),
                          QStringLiteral("d")};
    QList<qreal> centers;
    for (const QString& id : ids)
        centers.append(labels.value(id).center().x());
    for (int index = 1; index < centers.size(); ++index)
        if (centers.at(index) <= centers.at(index - 1))
            return {};

    QHash<QString, QJsonObject> result;
    const qreal rowY = labels.value(QStringLiteral("a")).center().y();
    for (int index = 0; index < ids.size(); ++index) {
        const qreal spacingLeft = index > 0 ? centers.at(index) - centers.at(index - 1)
                                            : centers.at(1) - centers.at(0);
        const qreal spacingRight = index + 1 < centers.size()
            ? centers.at(index + 1) - centers.at(index) : centers.at(index) - centers.at(index - 1);
        const qreal left = index > 0 ? (centers.at(index - 1) + centers.at(index)) / 2.0
                                     : centers.at(index) - spacingRight * 0.48;
        const qreal right = index + 1 < centers.size()
            ? (centers.at(index) + centers.at(index + 1)) / 2.0
            : centers.at(index) + spacingLeft * 0.48;
        // 选项图片位于标签正上方。高度刻意很窄，只带图片和 A/B/C/D 标签，不会
        // 把题干或相邻题目卷进选项；整个过程完全以 PDF 文字层坐标定位，不做 OCR。
        const QRectF normalizedCrop(left, qMax<qreal>(0.0, rowY - 0.065),
                                   right - left, 0.09);
        const QRect crop(qBound(0, qFloor(normalizedCrop.x() * source.width()), source.width() - 1),
                         qBound(0, qFloor(normalizedCrop.y() * source.height()), source.height() - 1),
                         qMax(1, qCeil(normalizedCrop.width() * source.width())),
                         qMax(1, qCeil(normalizedCrop.height() * source.height())));
        const QImage optionImage = source.copy(crop.intersected(source.rect()));
        if (optionImage.isNull())
            return {};
        QByteArray png;
        QBuffer buffer(&png);
        if (!buffer.open(QIODevice::WriteOnly) || !optionImage.save(&buffer, "PNG"))
            return {};
        const QString path = QStringLiteral("assets/%1-p%2-q%3-%4.png")
            .arg(assetBaseName(document.sourcePath)).arg(page).arg(number).arg(ids.at(index));
        assets->insert(path, png);
        result.insert(ids.at(index), visualAssetDescriptor(document, page, normalizedCrop, path,
            QStringLiteral("选项%1").arg(ids.at(index).toUpper())));
    }
    return result;
}

QRectF nextQuestionBoundsFor(const ExtractedDocument& document, int page, int number) {
    const QRectF current = questionBoundsFor(document, page, number);
    if (current.isEmpty())
        return {};
    QRectF next;
    for (const PdfTextAnchor& anchor : document.questionAnchors.value(page)) {
        if (anchor.bounds.top() <= current.bottom())
            continue;
        if (next.isEmpty() || anchor.bounds.top() < next.top())
            next = anchor.bounds;
    }
    return next;
}

QJsonObject extractQuestionVisualImage(const ExtractedDocument& document, int page, int number,
                                       const QStringList& stemSourceLines,
                                       bool isStemIllustration,
                                       QHash<QString, QByteArray>* assets) {
    if (!document.pageImages.contains(page))
        return {};
    const QImage source = QImage::fromData(document.pageImages.value(page), "PNG");
    const QRectF questionBounds = questionBoundsFor(document, page, number);
    if (source.isNull() || questionBounds.isEmpty())
        return {};
    // 图形题的 A/B/C/D 常是矢量字而非文字层，不能从不存在的字母坐标硬切。
    // 此时保留“本题题干 + 图阵 + 四个图形选项”的完整视觉区，并以下一题题号
    // 为不可越过的下边界，绝不回退为整页截图。
    const QRectF nextBounds = nextQuestionBoundsFor(document, page, number);
    const QRectF fullQuestionLineBounds = stemBoundsFor(document, page, stemSourceLines);
    const QRectF visualStartBounds =
        fullQuestionLineBounds.isEmpty() ? questionBounds : fullQuestionLineBounds;
    // 题干已有结构化文字，截图从题干行下方开始，只保留题目所需的图阵/公式/
    // 图形选项。这样既不会重复截入题干，也不会带入上一题的 A/B/C/D 标签。
    qreal top = qMin<qreal>(0.985, visualStartBounds.bottom() + 0.003);
    const int continuationPage = page + 1;
    const QRectF continuationNextBounds = nextBounds.isEmpty()
        ? questionBoundsFor(document, continuationPage, number + 1) : QRectF{};
    const bool spansNextPage = nextBounds.isEmpty() && !continuationNextBounds.isEmpty() &&
        document.pageImages.contains(continuationPage);
    qreal bottom = !nextBounds.isEmpty()
        // 只在下一题题号上方保留极小安全边距。此前 1.4% 页高的边距会把
        // 紧贴下一题的分数选项分母和 A/B/C/D 标签一起切掉。
        ? qMax(top, nextBounds.top() - 0.003)
        : spansNextPage ? 0.985 : qMin<qreal>(0.985, questionBounds.top() + 0.34);
    // 分数/公式选项的 A/B/C/D 文字锚点可靠时，直接框住这一整行视觉选项，
    // 不再把题干续行一起带入（北京卷第 76 题）。
    const QHash<QString, QRectF> optionRow = optionRowForQuestion(document, page, number);
    qreal cropLeft = 0.05;
    qreal cropRight = 0.95;
    if (optionRow.size() == 4 && !isStemIllustration) {
        const qreal rowY = optionRow.value(QStringLiteral("a")).center().y();
        // 公式/分数位于字母标签正上方。作答按钮本身会显示 A/B/C/D，因此截图
        // 只保留公式，不再冒险截标签基线：北京卷 76 的下一题紧贴标签下方，
        // 多留几个像素就会把第 77 题的文字卷进来。
        // 公式、分数或图形符号的主体常比 A/B/C/D 标签高出 6% 以上页高。此前
        // 只回退 2.8% 会把分子、根号或图形顶端切掉；保留约 9% 后仍以前一题
        // 题号和当前标签行为硬边界，不会把下一题带进截图。
        top = qMax<qreal>(0.0, rowY - 0.090);
        const qreal optionBottom = qMin<qreal>(0.995, rowY - 0.002);
        bottom = !nextBounds.isEmpty()
            ? qMin(optionBottom, nextBounds.top() - 0.001) : optionBottom;
        const qreal firstCenter = optionRow.value(QStringLiteral("a")).center().x();
        const qreal lastCenter = optionRow.value(QStringLiteral("d")).center().x();
        const qreal spacing = (lastCenter - firstCenter) / 3.0;
        cropLeft = qMax<qreal>(0.0,
            optionRow.value(QStringLiteral("a")).left() - spacing * 0.10);
        cropRight = qMin<qreal>(1.0,
            optionRow.value(QStringLiteral("d")).right() + spacing * 0.55);
    } else if (isStemIllustration) {
        // 题干明确写有“如图所示”时，这一行 A/B/C/D 是普通文本选项的上边界，
        // 图本身位于题干与选项之间。不能套用“公式选项”模式的窄条裁切，否则会
        // 截掉示意图顶部（专项智能练习第 1 题正是这种版式）。
        const qreal optionTop = firstOptionLabelTopForQuestion(document, page, number);
        if (optionTop < std::numeric_limits<qreal>::max()) {
            const qreal optionBottom = qMax(top, optionTop - 0.004);
            bottom = !nextBounds.isEmpty()
                ? qMin(optionBottom, nextBounds.top() - 0.003) : optionBottom;
        }
    }
    const qreal minimumVisualHeight = optionRow.size() == 4 ? 0.020 : 0.035;
    if (bottom <= top + minimumVisualHeight)
        return {};
    const QRectF normalizedCrop(cropLeft, top, cropRight - cropLeft, bottom - top);
    const QRect crop(qFloor(source.width() * normalizedCrop.x()),
                     qFloor(source.height() * normalizedCrop.y()),
                     qCeil(source.width() * normalizedCrop.width()),
                     qCeil(source.height() * normalizedCrop.height()));
    const auto trimTransparentMargins = [](const QImage& image) {
        if (image.isNull()) return QImage{};
        const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
        int left = argb.width(), topEdge = argb.height(), right = -1, bottomEdge = -1;
        for (int y = 0; y < argb.height(); ++y) {
            const QRgb* row = reinterpret_cast<const QRgb*>(argb.constScanLine(y));
            for (int x = 0; x < argb.width(); ++x) {
                if (qAlpha(row[x]) <= 8) continue;
                left = qMin(left, x);
                right = qMax(right, x);
                topEdge = qMin(topEdge, y);
                bottomEdge = qMax(bottomEdge, y);
            }
        }
        if (right < left || bottomEdge < topEdge) return QImage{};
        constexpr int padding = 4;
        const QRect content(qMax(0, left - padding), qMax(0, topEdge - padding),
                            qMin(argb.width() - 1, right + padding) - qMax(0, left - padding) + 1,
                            qMin(argb.height() - 1, bottomEdge + padding) -
                                qMax(0, topEdge - padding) + 1);
        return argb.copy(content);
    };
    QImage snippet = trimTransparentMargins(source.copy(crop.intersected(source.rect())));
    if (spansNextPage && !snippet.isNull()) {
        const QImage continuationSource =
            QImage::fromData(document.pageImages.value(continuationPage), "PNG");
        const qreal continuationTop = 0.015;
        const qreal continuationBottom =
            qMax(continuationTop, continuationNextBounds.top() - 0.003);
        if (!continuationSource.isNull() && continuationBottom > continuationTop + 0.01) {
            const QRect continuationCrop(
                qFloor(continuationSource.width() * 0.05),
                qFloor(continuationSource.height() * continuationTop),
                qCeil(continuationSource.width() * 0.90),
                qCeil(continuationSource.height() *
                      (continuationBottom - continuationTop)));
            const QImage continuation = trimTransparentMargins(continuationSource.copy(
                continuationCrop.intersected(continuationSource.rect())));
            if (!continuation.isNull()) {
                if (snippet.isNull()) {
                    snippet = continuation;
                } else {
                QImage combined(qMax(snippet.width(), continuation.width()),
                                snippet.height() + continuation.height(),
                                QImage::Format_ARGB32_Premultiplied);
                combined.fill(Qt::transparent);
                QPainter painter(&combined);
                painter.drawImage(0, 0, snippet);
                painter.drawImage(0, snippet.height(), continuation);
                painter.end();
                snippet = combined;
                }
            }
        }
    }
    QByteArray png;
    QBuffer buffer(&png);
    if (snippet.isNull() || !buffer.open(QIODevice::WriteOnly) || !snippet.save(&buffer, "PNG"))
        return {};
    const QString path = QStringLiteral("assets/%1-p%2-q%3.png")
        .arg(assetBaseName(document.sourcePath)).arg(page).arg(number);
    assets->insert(path, png);
    return visualAssetDescriptor(document, page, normalizedCrop, path,
        isStemIllustration ? QStringLiteral("原卷题图（题干插图）")
                            : QStringLiteral("原卷题图（含图形选项）"));
}

// 把任意选项标记归一化成小写字母 a/b/c…。支持：字母 A-I/a-i（含全角）、圈码
// ①②③④、带点数字 ⒈⒉、以及括号内/带尾括号的数字 (1)/1) 等。括号兼容全角/半角
// 与各种书写习惯：（ ( [ { 「 『 … ） ) ] } 」 』。
// 注意：用 NFC 而非 NFKC，NFKC 会把圈码 ① 分解成裸 1 而丢失选项标记。
QString normalizeOptionLabel(const QString& raw) {
    return canonicalOptionLabel(raw);
}

// 把答案文本归一化成大写字母串（如 "AC"）。支持字母、圈码、带点数字、
// 括号数字等多种写法。无法识别返回空。
QString normalizeAnswer(QString answer) {
    answer = answer.normalized(QString::NormalizationForm_C).trimmed();
    static const QRegularExpression separators(QStringLiteral("[\\s,，、;；/|+]+"));
    static const QRegularExpression closingBracket(QStringLiteral("[)）\\]}」』]"));
    answer.remove(separators);
    if (answer.isEmpty())
        return {};
    // 逐标记归一化：单字符（字母/圈码/带点数字）或括号数字片段 “(1)”。
    QString normalized;
    for (int k = 0; k < answer.size();) {
        const QChar ch = answer.at(k);
        if (ch == u'(' || ch == u'（' || ch == u'[' || ch == u'{' ||
            ch == u'「' || ch == u'『') {
            int close = answer.indexOf(closingBracket, k + 1);
            if (close > k) {
                const QString label = normalizeOptionLabel(answer.mid(k, close - k + 1));
                if (!label.isEmpty() && !normalized.contains(label.toUpper()))
                    normalized += label.toUpper();
                k = close + 1;
                continue;
            }
        }
        const QString label = normalizeOptionLabel(QString(ch));
        if (!label.isEmpty() && !normalized.contains(label.toUpper()))
            normalized += label.toUpper();
        ++k;
    }
    return normalized;
}

// 判断题答案归一化：把常见判断写法映射到合成选项 A(正确)/B(不正确)。覆盖
// 对/错、正确/错误、√/×、是/否。无法识别返回空。中文卷判断题几乎不用 T/F，
// 为避免把残片答案（如落单的 "F"）误判成判断题，这里不收 T/F/Y/N 单字母。
QString booleanAnswerLabel(const QString& raw) {
    const QString a = raw.normalized(QString::NormalizationForm_KC).trimmed();
    if (a.isEmpty())
        return {};
    const auto containsAny = [&a](const QStringList& list) {
        for (const QString& token : list)
            if (a.contains(token))
                return true;
        return false;
    };
    // 先判否定：否定写法都不含肯定写法，顺序安全（“错误”不含“对/正确/是”）。
    if (containsAny({QStringLiteral("错误"), QStringLiteral("错"), QStringLiteral("×"),
                     QStringLiteral("✗"), QStringLiteral("✕"), QStringLiteral("否")}))
        return QStringLiteral("B");
    if (containsAny({QStringLiteral("正确"), QStringLiteral("对"), QStringLiteral("√"),
                     QStringLiteral("是")}))
        return QStringLiteral("A");
    return {};
}

// 判断题题干识别：以空括号“（ ）”结尾（允许内部半角/全角空白）。这类题不列
// A/B 选项，答案另写 对/错，需要据此合成“正确/不正确”两个选项。
bool hasBlankJudgmentBrackets(const QString& stem) {
    static const QRegularExpression pattern(
        QStringLiteral(R"([（(][\s　]*[)）]\s*$)"));
    return pattern.match(stem.trimmed()).hasMatch();
}

QString comparableText(QString value) {
    value = value.normalized(QString::NormalizationForm_KC).toLower().trimmed();
    static const QRegularExpression noise(QStringLiteral("[\\s\\p{P}\\p{S}]+"));
    value.remove(noise);
    return value;
}

QString answerFromOptionText(const QString& rawAnswer,
                             const QList<QPair<QString, QString>>& options) {
    const QString direct = normalizeAnswer(rawAnswer);
    if (!direct.isEmpty())
        return direct;
    const QString expected = comparableText(rawAnswer);
    if (expected.isEmpty())
        return {};
    QString matched;
    for (const auto& option : options) {
        const QString actual = comparableText(option.second);
        if (actual == expected ||
            (expected.size() >= 4 && (actual.contains(expected) || expected.contains(actual)))) {
            if (!matched.isEmpty())
                return {}; // 多个近似选项时拒绝猜测。
            matched = option.first.toUpper();
        }
    }
    return matched;
}

QList<SourceLine> sourceLines(const ExtractedDocument& document) {
    QList<SourceLine> result;
    int page = document.hasPageBoundaries ? document.firstPageNumber : 0;
    QString current;
    bool paragraphBreakBefore = false;
    bool previousWasCarriageReturn = false;
    const QString normalized = document.plainText.normalized(QString::NormalizationForm_C);
    for (const QChar ch : normalized) {
        // PDF 文本层常用 CRLF。此前 \r 已经提交当前视觉行，紧随的 \n 又被当作
        // 一次“空行”，于是每一行都带上 paragraphBreakBefore，复核页看起来像
        // 每行之间都空了一段。CRLF 必须视为一个换行符。
        if (ch == u'\n' && previousWasCarriageReturn) {
            previousWasCarriageReturn = false;
            continue;
        }
        if (ch == u'\f') {
            if (!current.trimmed().isEmpty())
                result.append({current.trimmed(), page, paragraphBreakBefore});
            current.clear();
            paragraphBreakBefore = false;
            previousWasCarriageReturn = false;
            if (document.hasPageBoundaries)
                ++page;
        } else if (ch == u'\n' || ch == u'\r') {
            if (!current.isEmpty()) {
                QString cleaned = current.trimmed();
                // PDF 页脚通常是 "- 15 -" / "-15-"，绝不能进入题干；保留
                // 其它换行以免把资料题的段落硬拼成一行。
                static const QRegularExpression pageFooter(QStringLiteral(R"(^[-—–\s]*\d{1,4}[-—–\s]*$)"));
                if (!pageFooter.match(cleaned).hasMatch()) {
                    result.append({cleaned, page, paragraphBreakBefore});
                    paragraphBreakBefore = false;
                }
            } else {
                paragraphBreakBefore = true;
            }
            current.clear();
            previousWasCarriageReturn = ch == u'\r';
        } else {
            current += ch;
            previousWasCarriageReturn = false;
        }
    }
    if (!current.trimmed().isEmpty()) result.append({current.trimmed(), page, paragraphBreakBefore});
    return result;
}

// 只有明确标题后跟第一题才分套；目录标题和没有依据的重号不生成新作用域。
// 裸章节标题（“实战演练一”“第一部分”这类整行短标题）额外要求证据才肯切分：
// 标题后到下一标题之间的第一道题必须是第 1 题（题号重启），且同页靠前不能
// 出现“目录”（目录条目）。误切会把题目切碎，比不切更有害，因此宁缺毋滥。
QList<ExtractedDocument> splitBookletSections(const ExtractedDocument& document) {
    if (!document.sectionId.isEmpty()) return {document};
    static const QRegularExpression strongTitle(QStringLiteral(
        R"(专项刷题[一二三四五六七八九十百\d]+|第[一二三四五六七八九十百\d]+套(?:试题|试卷|练习题)?|(?:试卷|套题)[一二三四五六七八九十百\d]+)"));
    // 关键词只收高信号复合词，且必须带编号（“实战演练一”）：单独的
    // “练习/演练/测试”可能出现在题干里。“第X部分/章”允许跟一个题型名
    // （“第一部分 片段阅读”）；句子状语（“第一部分规定了……”）因后面还有
    // 其它文字而不会整行命中。
    static const QRegularExpression bareChapter(QStringLiteral(
        R"chapter((?:实战|模拟|专项|强化|基础|巩固|综合|同步|真题|单元)(?:演练|练习|训练|测试|刷题)(?:[一二三四五六七八九十]{1,3}|\d{1,2})|[一二三四五六七八九十]{1,3}(?:部分|章|节|卷|篇|组|单元)(?:\s+\S{1,12})?|第[一二三四五六七八九十百\d]{1,3}(?:部分|章|节|卷|篇|组|单元)(?:\s+\S{1,12})?)chapter"));
    const QStringList lines = document.plainText.split(QRegularExpression(QStringLiteral("[\\r\\n\\f]+")));
    // 一行可含多组答案对（“1.A 2.B”），整行是答案对时不算题号锚点。
    static const QRegularExpression answerPairLine(QStringLiteral(
        R"(^\s*(?:(?:第\s*)?\d{1,4}\s*(?:题|[\.．、:：\)）])\s*(?:【?答案】?\s*[:：]?)?\s*[A-Fa-f]{1,6}\s*)+\s*$)"));
    const auto questionNumberOn = [&lines](int index) {
        if (answerPairLine.match(lines.at(index).trimmed()).hasMatch())
            return 0;
        const auto match = questionPattern().match(lines.at(index));
        return match.hasMatch() ? match.captured(1).toInt() : 0;
    };
    // 各行起始在原文中的偏移；分隔符是 [\r\n\f]+（CRLF 为双字符），逐字符跳过。
    QList<int> lineOffsets;
    lineOffsets.reserve(lines.size());
    {
        int textPos = 0;
        for (int i = 0; i < lines.size(); ++i) {
            lineOffsets.append(textPos);
            textPos += lines.at(i).size();
            if (i + 1 < lines.size()) {
                while (textPos < document.plainText.size() &&
                       (document.plainText.at(textPos) == u'\r' ||
                        document.plainText.at(textPos) == u'\n' ||
                        document.plainText.at(textPos) == u'\f'))
                    ++textPos;
            }
        }
    }
    // 同页靠前出现“目录”的行按目录条目处理；换页符是页面的可靠分隔。
    const auto tocPageBefore = [&document, &lineOffsets](int line) {
        const int start = lineOffsets.at(line);
        const int before = document.plainText.left(start).lastIndexOf(QChar('\f'));
        const int pageStart = before < 0 ? 0 : before + 1;
        return document.plainText.mid(pageStart, start - pageStart).contains(QStringLiteral("目录"));
    };
    struct Candidate { int line = 0; QString text; bool bare = false; };
    QList<Candidate> form;
    // 标题必须独占整行：题干里出现“实战演练一”字样（“本题来自实战演练一”）
    // 绝不能被当成章节边界。
    const auto isFullLine = [](const QRegularExpression& pattern, const QString& text) {
        const auto match = pattern.match(text);
        return match.hasMatch() && match.captured(0) == text;
    };
    for (int i = 0; i < lines.size(); ++i) {
        const QString text = lines.at(i).trimmed();
        if (text.isEmpty() || text.size() > 24)
            continue;
        const bool strong = isFullLine(strongTitle, text);
        const bool bare = !strong && isFullLine(bareChapter, text);
        if (strong || bare)
            form.append({i, text, bare});
    }
    QList<Candidate> boundaries;
    for (int k = 0; k < form.size(); ++k) {
        const Candidate candidate = form.at(k);
        const int next = k + 1 < form.size() ? form.at(k + 1).line : lines.size();
        // 标题后（到下一标题前）的第一道题必须是第 1 题：目录行、题干里的短行
        // 以及“不重启编号”的分节标题都会在这里被挡掉。
        int firstAfter = 0;
        for (int j = candidate.line + 1; j < next; ++j) {
            firstAfter = questionNumberOn(j);
            if (firstAfter > 0)
                break;
        }
        if (firstAfter != 1)
            continue;
        if (candidate.bare && tocPageBefore(candidate.line))
            continue;
        boundaries.append(candidate);
    }
    if (boundaries.isEmpty()) return {document};
    QList<ExtractedDocument> result;
    // 第一份明确标题之前可能还有未命名的试题，不能当成封面/目录丢弃。
    const int prefixEnd = lineOffsets.at(boundaries.first().line);
    const QString prefix = document.plainText.left(prefixEnd);
    bool prefixQuestion = false, prefixOptions = false;
    for (const auto& line : prefix.split(QRegularExpression(QStringLiteral("[\\r\\n\\f]+")))) {
        prefixQuestion |= questionPattern().match(line).hasMatch();
        prefixOptions |= QRegularExpression(QStringLiteral(R"(^\s*[A-Fa-f]\s*[.．、:：)）])")).match(line).hasMatch();
    }
    if (prefixQuestion && prefixOptions) {
        ExtractedDocument ungrouped = document;
        ungrouped.sectionId = QStringLiteral("set-prefix");
        ungrouped.sectionTitle = QStringLiteral("未分套题目");
        ungrouped.plainText = prefix;
        result.append(ungrouped);
    }
    for (int i = 0; i < boundaries.size(); ++i) {
        const int start = lineOffsets.at(boundaries.at(i).line) + lines.at(boundaries.at(i).line).size();
        const int end = i + 1 < boundaries.size()
            ? lineOffsets.at(boundaries.at(i + 1).line)
            : document.plainText.size();
        ExtractedDocument section = document;
        section.sectionId = QStringLiteral("set-%1").arg(i + 1);
        section.sectionTitle = boundaries.at(i).text;
        section.firstPageNumber += document.plainText.left(start).count(QChar('\f'));
        section.plainText = document.plainText.mid(start, end - start);
        result.append(section);
    }
    return result;
}

QString companionAnswerTextForSection(const QString& answerText, const QString& sectionTitle,
                                      const QStringList& knownSectionTitles) {
    if (answerText.trimmed().isEmpty() || sectionTitle.isEmpty())
        return answerText;
    const QStringList lines = answerText.split(QRegularExpression(QStringLiteral("[\\r\\n\\f]+")));
    int offset = 0;
    int sectionStart = -1;
    int sectionEnd = answerText.size();
    for (const QString& line : lines) {
        const int lineStart = offset;
        offset += line.size();
        while (offset < answerText.size() &&
               (answerText.at(offset) == u'\r' || answerText.at(offset) == u'\n' ||
                answerText.at(offset) == u'\f'))
            ++offset;
        const QString title = line.trimmed();
        if (sectionStart < 0) {
            if (title == sectionTitle)
                sectionStart = offset;
            continue;
        }
        if (knownSectionTitles.contains(title)) {
            sectionEnd = lineStart;
            break;
        }
    }
    // 答案册没有与题本一致的套题标题时，保留全文交给题号/数量安全规则。
    return sectionStart >= 0 ? answerText.mid(sectionStart, sectionEnd - sectionStart)
                             : answerText;
}

QHash<int, QRectF> lineBoundsBySourceIndex(const ExtractedDocument& document,
                                            const QList<SourceLine>& lines) {
    QHash<int, QRectF> result;
    QHash<int, int> anchorCursors;
    for (int index = 0; index < lines.size(); ++index) {
        const SourceLine& source = lines.at(index);
        if (source.page <= 0 || !document.lineAnchors.contains(source.page))
            continue;
        const QList<PdfTextAnchor>& anchors = document.lineAnchors.value(source.page);
        int& cursor = anchorCursors[source.page];
        for (int anchorIndex = cursor; anchorIndex < anchors.size(); ++anchorIndex) {
            if (anchors.at(anchorIndex).text.simplified() != source.text.simplified())
                continue;
            result.insert(index, anchors.at(anchorIndex).bounds);
            cursor = anchorIndex + 1;
            break;
        }
    }
    return result;
}

bool isAsciiWordCharacter(QChar character) {
    return character.isLetterOrNumber() && character.unicode() < 128;
}

QString joinWrappedText(const QString& previous, const QString& next) {
    if (previous.isEmpty()) return next;
    if (next.isEmpty()) return previous;
    // 汉字/数字在 PDF 行尾被截断时通常不应补空格；只为英文单词的折行补一个。
    const bool needSpace = isAsciiWordCharacter(previous.back()) &&
        isAsciiWordCharacter(next.front());
    return previous + (needSpace ? QStringLiteral(" ") : QString()) + next;
}

QStringList reflowVisualLines(const ExtractedDocument& document,
                              const QList<SourceLine>& lines,
                              const QList<int>& sourceIndices,
                              const QStringList& textLines) {
    if (sourceIndices.size() != textLines.size() || textLines.isEmpty())
        return textLines;
    const QHash<int, QRectF> bounds = lineBoundsBySourceIndex(document, lines);
    QStringList paragraphs;
    QString paragraph;
    int previousIndex = -1;
    for (int offset = 0; offset < textLines.size(); ++offset) {
        const int sourceIndex = sourceIndices.at(offset);
        const QString text = textLines.at(offset).trimmed();
        if (text.isEmpty() || sourceIndex < 0 || sourceIndex >= lines.size())
            continue;
        bool newParagraph = paragraph.isEmpty() || lines.at(sourceIndex).paragraphBreakBefore;
        if (!newParagraph && previousIndex >= 0 && lines.at(previousIndex).page == lines.at(sourceIndex).page &&
            bounds.contains(previousIndex) && bounds.contains(sourceIndex)) {
            const QRectF previous = bounds.value(previousIndex);
            const QRectF current = bounds.value(sourceIndex);
            const qreal normalHeight = qMax<qreal>(0.004, qMax(previous.height(), current.height()));
            const qreal verticalGap = current.top() - previous.bottom();
            const qreal characterWidth = previous.width() /
                qMax(1, lines.at(previousIndex).text.size());
            const bool extraVerticalGap = verticalGap > normalHeight * 0.85;
            const bool firstLineIndent = current.left() - previous.left() >
                qMax<qreal>(0.012, characterWidth * 1.25);
            newParagraph = extraVerticalGap || firstLineIndent;
        }
        if (newParagraph) {
            if (!paragraph.isEmpty()) paragraphs.append(paragraph);
            paragraph = text;
        } else {
            paragraph = joinWrappedText(paragraph, text);
        }
        previousIndex = sourceIndex;
    }
    if (!paragraph.isEmpty()) paragraphs.append(paragraph);
    return paragraphs;
}

constexpr QChar kUnderlineStartMarker(0xe000);
constexpr QChar kUnderlineEndMarker(0xe001);

struct MaterialTextWithDecorations {
    QString body;
    QJsonArray underlines;
};

void recoverTrailingMineruBlank(MaterialTextWithDecorations* stem) {
    if (!stem || stem->body.contains(QStringLiteral("〔填空〕")) ||
        !stem->underlines.isEmpty())
        return;
    static const QRegularExpression instruction(QStringLiteral(
        R"((?:\n|^)?\s*(?:依次)?填入[^\n]{0,20}(?:横线|划横线|画横线)[^\n]*$)"));
    const auto cue = instruction.match(stem->body);
    if (!cue.hasMatch())
        return;
    int boundary = cue.capturedStart();
    while (boundary > 0 && stem->body.at(boundary - 1).isSpace())
        --boundary;
    if (boundary <= 0)
        return;

    // 仅处理 MinerU 已知的三种窄形态：句尾横线被彻底删掉、被幻读成单个
    // ASCII 字符，或引号内横线被幻读成逗号。必须紧邻“填入横线”提示，绝不
    // 在普通正文中凭语义猜测空白位置。
    const QChar last = stem->body.at(boundary - 1);
    if ((last == u',' || last == u'，') && boundary >= 2 &&
        (stem->body.at(boundary - 2) == u'“' || stem->body.at(boundary - 2) == u'\"')) {
        stem->body.replace(boundary - 1, 1, QStringLiteral("〔填空〕”"));
    } else if (last == u',' || last == u'，') {
        stem->body.insert(boundary, QStringLiteral("〔填空〕"));
    } else if (last.isLetterOrNumber() && last.unicode() < 128 && boundary >= 2 &&
               stem->body.at(boundary - 2).script() == QChar::Script_Han) {
        stem->body.replace(boundary - 1, 1, QStringLiteral("〔填空〕"));
    }
}

MaterialTextWithDecorations buildMaterialText(const ExtractedDocument& document,
                                              const QList<SourceLine>& lines,
                                              const QList<int>& sourceIndices,
                                              const QStringList& bodyLines,
                                              bool material = true) {
    QStringList markedLines;
    markedLines.reserve(bodyLines.size());
    for (int index = 0; index < bodyLines.size(); ++index) {
        const QString text = bodyLines.at(index);
        const SourceLine& source = lines.at(sourceIndices.at(index));
        const int offset = source.text.indexOf(text);
        QString marked;
        bool found = false;
        for (const auto& decoration : document.underlineDecorations.value(source.page)) {
            if (offset < 0 || decoration.text != source.text) continue;
            bool active = false;
            for (int i = 0; i <= text.size(); ++i) {
                bool underlined = false;
                for (const auto& range : decoration.ranges)
                    if (i < text.size() && i + offset >= range.first &&
                        i + offset < range.first + range.second) underlined = true;
                if (active != underlined) marked += underlined ? kUnderlineStartMarker : kUnderlineEndMarker;
                active = underlined;
                bool blank = false;
                for (const auto& range : decoration.blanks) {
                    // 题号后的空格会被 firstStemLine.trimmed() 去掉，但该空格可能
                    // 承载真实的句首填空，必须保留几何证据对应的零宽插入位置。
                    const int begin = qMax(0, range.first - offset);
                    const int end = range.first + range.second - offset;
                    if (end >= 0 && i == begin && end <= text.size()) {
                        marked += QStringLiteral("〔填空〕");
                        if (end > i) { i = end - 1; blank = true; }
                        break;
                    }
                }
                if (!blank && i < text.size()) marked += text.at(i);
            }
            found = true;
            break;
        }
        markedLines.append(found ? marked : text);
    }
    QString marked = reflowVisualLines(document, lines, sourceIndices, markedLines)
        .join(material ? QStringLiteral("\n\n") : QStringLiteral("\n")).trimmed();
    if (material) marked = restoreDroppedBlankLines(marked);
    else marked.replace(QRegularExpression(QStringLiteral("(?:_{2,}|＿{2,})")), QStringLiteral("〔填空〕"));
    MaterialTextWithDecorations result;
    int underlineStart = -1;
    for (const QChar character : marked) {
        if (character == kUnderlineStartMarker) {
            underlineStart = result.body.size();
        } else if (character == kUnderlineEndMarker) {
            if (underlineStart >= 0 && result.body.size() > underlineStart) {
                const int length = result.body.size() - underlineStart;
                if (!result.underlines.isEmpty()) {
                    QJsonObject previous = result.underlines.last().toObject();
                    if (previous.value(QStringLiteral("start")).toInt() +
                            previous.value(QStringLiteral("length")).toInt() == underlineStart) {
                        previous.insert(QStringLiteral("length"),
                                        previous.value(QStringLiteral("length")).toInt() + length);
                        result.underlines.last() = previous;
                    } else {
                        result.underlines.append(QJsonObject{{QStringLiteral("start"), underlineStart},
                                                             {QStringLiteral("length"), length}});
                    }
                } else {
                    result.underlines.append(QJsonObject{{QStringLiteral("start"), underlineStart},
                                                         {QStringLiteral("length"), length}});
                }
            }
            underlineStart = -1;
        } else {
            result.body += character;
        }
    }
    return result;
}

// 一个答案区头之后的作用域终点：遇到下一个题号锚点行或下一个材料头行就
// 停止。这样阶段分组的文件里，阶段二的题目不会被前一阶段的答案区吞掉，而
// 是被重新识别为题目。两个集合都按行号升序传入。
int answerSectionEnd(const QList<SourceLine>& lines, int sectionStart,
                     const QList<QuestionAnchor>& anchors,
                     const QList<MaterialMarker>& materials) {
    const int count = lines.size();
    int end = count;
    for (const auto& anchor : anchors) {
        if (anchor.line > sectionStart && anchor.line < end) {
            end = anchor.line;
            break;
        }
    }
    for (const auto& material : materials) {
        if (material.line > sectionStart && material.line < end)
            end = material.line;
    }
    return end;
}

// includeSectionStartLine：答案区头与答案串排在同一行时（“【参考答案】CAACD…”），
// 标题行本身携带答案，必须纳入扫描；标题独占一行时跳过它，避免把标题文字
// 当作答案 token。
QString sortedAnswer(QString answer) {
    std::sort(answer.begin(), answer.end());
    return answer;
}

QHash<int, QString> globalAnswers(const QList<SourceLine>& lines, int sectionStart, int sectionEnd,
                                  bool includeSectionStartLine,
                                  QSet<int>* ambiguousNumbers,
                                  QList<QPair<int, QString>>* orderedRecords = nullptr) {
    QHash<int, QString> answers;
    const auto recordAnswer = [&](int number, const QString& answer) {
        if (number <= 0 || answer.isEmpty() || ambiguousNumbers->contains(number)) return;
        if (answers.contains(number) && sortedAnswer(answers.value(number)) != sortedAnswer(answer)) {
            ambiguousNumbers->insert(number);
            answers.remove(number);
        } else {
            answers.insert(number, answer);
            if (orderedRecords)
                orderedRecords->append({number, answer});
        }
    };
    if (sectionStart < 0)
        return answers;
    const int limit = sectionEnd >= 0 ? sectionEnd : lines.size();
    const int scanStart = includeSectionStartLine ? sectionStart : sectionStart + 1;
    // 表格化答案区：形如
    //   题号 | 1 | 2 | 3
    //   答案 | A | B | C
    // 或 markdown | 1 | 2 | 3 | 与 | A | B | C |。按列对齐配对。
    static const QRegularExpression cell(QStringLiteral("[^|｜\\s,，]+"));
    // 下面四个临时正则原本写在循环里逐行构造，每行都重新编译一次模式。提到
    // static const 后只编译一次，整份资料扫描的热路径显著变快。
    static const QRegularExpression pipeMarker(QStringLiteral("[|｜]"));
    static const QRegularExpression numberHeader(QStringLiteral("题号|题"));
    static const QRegularExpression answerHeader(QStringLiteral("答案|答"));
    for (int index = scanStart; index < limit && index < lines.size(); ++index) {
        const QString a = lines.at(index).text.trimmed();
        if (a.isEmpty() || !a.contains(pipeMarker))
            continue;
        if (!a.contains(numberHeader))
            continue;
        // 下一非空行作为答案行。
        int answerLine = index + 1;
        while (answerLine < limit && answerLine < lines.size() &&
               lines.at(answerLine).text.trimmed().isEmpty())
            ++answerLine;
        if (answerLine >= lines.size())
            break;
        const QString b = lines.at(answerLine).text.trimmed();
        if (!b.contains(pipeMarker) || !b.contains(answerHeader))
            continue;
        // 抽取两行的非分隔单元格，按列配对（题号行第一格“题号”是表头，跳过）。
        auto cells = [](const QString& row) {
            QList<QString> out;
            auto it = cell.globalMatch(row);
            while (it.hasNext())
                out.append(it.next().captured(0));
            return out;
        };
        const QList<QString> numCells = cells(a);
        const QList<QString> ansCells = cells(b);
        // 去掉表头后逐列配对。
        const int offset = 1;
        for (int col = offset; col < numCells.size() && col < ansCells.size(); ++col) {
            bool ok = false;
            const int number = numCells.at(col).toInt(&ok);
            if (!ok || number <= 0)
                continue;
            const QString answer = normalizeAnswer(ansCells.at(col));
            recordAnswer(number, answer);
        }
        index = answerLine; // 跳过已消费的答案行
    }
    // 答案 token：字母 A-F（大小写）、圈码 ①-⑩、或带括号/带点的数字。捕获后
    // 统一经 normalizeAnswer 归一化，兼容选项是圈码或数字括号的答案区。
    const QString answerToken(QStringLiteral(
        R"([A-Fa-f](?:\s*[,，、;；/|+]\s*[A-Fa-f]){0,5}|[A-Fa-f]{1,6}|[①②③④⑤⑥⑦⑧⑨⑩]{1,6}|[（(\[{「『]\s*[1-9]\s*[)）\]}」』]|[1-9]\s*[)）\]」』])"));
    const QString pairPattern =
        QStringLiteral(R"((?:^|[\s,，;；])(?:第\s*)?(\d{1,4})\s*(?:题|[\.．、:：\)）-])?\s*(?:【?答案】?\s*[:：]?)?\s*()") +
        answerToken +
        QStringLiteral(R"()(?=$|[\s,，;；]))");
    static const QRegularExpression pair(pairPattern);
    static const QRegularExpression range(
        QStringLiteral(R"((\d{1,4})\s*(?:[-—~～]|至|到)\s*(\d{1,4})\s*[:：]?\s*([A-Fa-f①②③④⑤⑥⑦⑧⑨⑩]+))"));
    static const QRegularExpression answerRecord(
        QStringLiteral(R"(^\s*(?:第\s*)?(\d{1,4})\s*(?:题|[．、:：\)）]|\.(?!\d))\s*)"));
    const QString narrativePattern =
        QStringLiteral(R"((?:故\s*)?(?:(?:正确|参考|标准)\s*)?答案\s*(?:为|是|[:：])\s*()") +
        answerToken +
        QStringLiteral(R"())");
    static const QRegularExpression narrativeAnswer(narrativePattern);
    int currentNumber = 0;
    const int lineLimit = sectionEnd >= 0 ? sectionEnd : lines.size();
    for (int index = scanStart; index < lineLimit && index < lines.size(); ++index) {
        const QString line = lines.at(index).text;
        const auto record = answerRecord.match(line);
        if (record.hasMatch()) currentNumber = record.captured(1).toInt();
        // 一行可含多组区间答案（“1~5: DDCBC  6~10: CBCAD  11~15: DCDCD”），
        // 逐组展开；只取第一组会丢掉后半卷的答案。
        auto rangeMatches = range.globalMatch(line);
        while (rangeMatches.hasNext()) {
            const auto rangeMatch = rangeMatches.next();
            const int first = rangeMatch.captured(1).toInt();
            const int last = rangeMatch.captured(2).toInt();
            const QString rawValues = rangeMatch.captured(3);
            // 逐字符归一化（兼容字母与圈码答案），长度与题号数相同时才展开。
            QString values;
            for (const QChar& ch : rawValues) {
                const QString one = normalizeAnswer(QString(ch));
                if (!one.isEmpty())
                    values += one;
            }
            if (last >= first && last - first + 1 == values.size()) {
                for (int number = first; number <= last; ++number)
                    recordAnswer(number, QString(values.at(number - first)));
            }
        }
        auto matches = pair.globalMatch(line);
        while (matches.hasNext()) {
            const auto match = matches.next();
            const int number = match.captured(1).toInt();
            const QString answer = normalizeAnswer(match.captured(2));
            const QString trailing = line.mid(match.capturedEnd()).trimmed();
            if (trailing.startsWith(QStringLiteral("项")) ||
                trailing.startsWith(QStringLiteral("选项")))
                continue;
            recordAnswer(number, answer);
        }
        // 判断题答案行：形如 “1.√”“2.×”“1.对”“2.错误”。对/错/√/× 不在选择题答案
        // token 内，单独匹配并归一化到合成选项 a(正确)/b(不正确)。一行可含多道，
        // 如 “1.√ 2.×”。
        static const QRegularExpression booleanRecord(QStringLiteral(
            R"((?:^|[\s,，;；])(?:第\s*)?(\d{1,4})\s*(?:题|[\.．、:：\)）])?\s*([√×✓✗✕对错是否正确错误]+))"));
        auto booleanMatches = booleanRecord.globalMatch(line);
        while (booleanMatches.hasNext()) {
            const auto match = booleanMatches.next();
            const int number = match.captured(1).toInt();
            const QString label = booleanAnswerLabel(match.captured(2));
            recordAnswer(number, label);
        }
        // 很多真题解析不是“1. A”式答案汇总，而是在题目解析末尾写“故正确
        // 答案为 C”。用最近一个题号归属这条结论，兼容题目文件与答案文件分离。
        const auto narrative = narrativeAnswer.match(line);
        if (currentNumber > 0 && narrative.hasMatch()) {
            const QString answer = normalizeAnswer(narrative.captured(1));
            recordAnswer(currentNumber, answer);
        }
    }
    return answers;
}

// 连续答案串：形如“CAACD DBABC BBCCD DABAC”，整段只有字母、没有任何题号。
// 真题的末页答案页几乎都是这种排版，而按题号配对的规则对它完全无能为力。
// 这里只负责把字母抽成有序序列，是否采用交由调用方按“数量必须与题号完全
// 相等”判断——错位写入比不写入更有害。
QStringList contiguousAnswerRun(const QString& text) {
    static const QRegularExpression stripHeader(QStringLiteral(
        R"(^\s*【?\s*(?:答案|参考答案|答案汇总|答案及解析|参考答案及解析)\s*】?\s*[:：]?\s*)"));
    QString body = text;
    body.remove(stripHeader);
    if (body.trimmed().isEmpty())
        return {};
    QStringList answers;
    for (const QChar& ch : body) {
        if (ch.isSpace())
            continue;
        // 出现字母与空白之外的任何字符（数字、标点、汉字）说明这不是纯答案串，
        // 例如“答案见解析”“1.A 2.B”。整行放弃，交给按题号的规则处理。
        const QString one = normalizeAnswer(QString(ch));
        if (one.isEmpty())
            return {};
        answers.append(one);
    }
    return answers;
}

QHash<int, QString> globalSolutions(const QList<SourceLine>& lines, int sectionStart, int sectionEnd) {
    QHash<int, QString> solutions;
    if (sectionStart < 0)
        return solutions;
    static const QRegularExpression record(QStringLiteral(
        R"(^\s*(?:第\s*)?(\d{1,4})\s*(?:题|[\.．、\)）])\s*(?:【?答案】?\s*[:：]?)?\s*[A-Fa-f]{1,6}\s*(.*)$)"));
    int currentNumber = 0;
    bool collecting = false;
    const int limit = sectionEnd >= 0 ? sectionEnd : lines.size();
    for (int index = sectionStart + 1; index < limit && index < lines.size(); ++index) {
        const QString line = lines.at(index).text.trimmed();
        const auto recordMatch = record.match(line);
        if (recordMatch.hasMatch()) {
            currentNumber = recordMatch.captured(1).toInt();
            collecting = false;
            QString tail = recordMatch.captured(2).trimmed();
            const auto solutionMatch = solutionPattern().match(tail);
            if (solutionMatch.hasMatch()) {
                tail = solutionMatch.captured(1).trimmed();
                collecting = true;
            } else {
                const int marker = tail.indexOf(
                    QRegularExpression(QStringLiteral("(?:【?解析】?|答案解析)\\s*[:：]?")));
                if (marker >= 0) {
                    tail = tail.mid(marker).replace(QRegularExpression(QStringLiteral(
                                                        "^(?:【?解析】?|答案解析)\\s*[:：]?\\s*")),
                                                    {});
                    collecting = true;
                }
            }
            if (collecting && !tail.isEmpty())
                solutions.insert(currentNumber, tail);
            continue;
        }
        const auto solutionMatch = solutionPattern().match(line);
        if (solutionMatch.hasMatch() && currentNumber > 0) {
            collecting = true;
            const QString first = solutionMatch.captured(1).trimmed();
            if (!first.isEmpty())
                solutions.insert(currentNumber, first);
            continue;
        }
        if (collecting && currentNumber > 0 && !line.isEmpty()) {
            QString value = solutions.value(currentNumber);
            if (!value.isEmpty())
                value += QChar('\n');
            solutions.insert(currentNumber, value + line);
        }
    }
    return solutions;
}

// 只匹配成对括号内的纯选项字母；中文解释、数字、公式等不在候选范围内。
// 捕获组 2 包含括号内部的全部空白，便于清理时保留原括号而不残留答案。
QList<QRegularExpressionMatch> bracketAnswerCandidates(const QString& text) {
    static const QRegularExpression pattern(QStringLiteral(
        R"(([（(\[{「『])(\s*[A-Fa-fＡ-Ｆａ-ｆ](?:[\s,，、;；/|+]*[A-Fa-fＡ-Ｆａ-ｆ]){0,5}\s*)([）)\]}」』]))"));
    const QString opens = QStringLiteral("（([{「『");
    const QString closes = QStringLiteral("）)]}」』");
    QList<QRegularExpressionMatch> result;
    auto matches = pattern.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        if (opens.indexOf(match.captured(1)) == closes.indexOf(match.captured(3)))
            result.append(match);
    }
    return result;
}

// 切分一行内的选项。返回选项列表（label 已归一化为 a/b/c…）与首个选项 marker
// 之前的题干前缀。支持的 marker 形式：字母 `A.`、圈码 `①`、带点数字 `⒈`、
// 括号数字 `(1)` 与尾括号数字 `1)`。
// - 仅 1 个 marker 且 marker 不在行首：拒绝（避免把含“B.”的英文题干误切）。
// - ≥2 个 marker：即使 marker 前有题干文字也切分，前缀文本经 *prefix 回传给
//   parseQuestion 合并进题干（覆盖“1. 题干 A.甲 B.乙 C.丙 D.丁”单行写法）。
// - 1 个 marker 且在行首：原纯选项行行为。
QList<QPair<QString, QString>> optionsOnLine(const QString& line, QString* prefix = nullptr,
                                             bool allowNumericLabels = true) {
    // 各 marker 捕获其 label 原文，后续统一归一化。捕获组按出现顺序：
    // 1=字母, 2=圈码, 3=括号数字 (1), 4=尾括号数字 1)。
    // 括号兼容全角/半角与各种书写习惯：（ ( [ { 「 『 … ） ) ] } 」 』。
    // 选项字母大小写都接受（A-F / a-f）。
    static const QRegularExpression marker(QStringLiteral(
        R"((?<![A-Za-z0-9])([A-Fa-f])\s*[\.．、:：\)）]\s*)"            // 字母 + 分隔（分隔必需）
        R"(|(?<![A-Za-z0-9])([A-Fa-f])\s+(?=[^\s]))"                  // 字母 + 空格（无标点题库）
        R"(|([①②③④⑤⑥⑦⑧⑨⑩])\s*[:：\.．、\)）\]」』]?\s*)"          // 圈码 + 可选分隔
        R"(|([①②③④⑤⑥⑦⑧⑨⑩])\s+)"                                  // 圈码 + 空格分隔
        R"(|[（(\[{「『]\s*([1-9])\s*[)）\]}」』]\s*)"                  // (1) 形式（各种括号）
        R"(|(?<![A-Za-z0-9])([1-9])\s*[)）\]」』]\s*)"));               // 1) 形式
    QList<QRegularExpressionMatch> markers;
    const auto brackets = bracketAnswerCandidates(line);
    auto iterator = marker.globalMatch(line);
    while (iterator.hasNext()) {
        const auto match = iterator.next();
        bool inBracket = false;
        for (const auto& bracket : brackets)
            if (match.capturedStart() >= bracket.capturedStart() &&
                match.capturedStart() < bracket.capturedEnd()) { inBracket = true; break; }
        if (inBracket) continue; // “（ A ）”不是选项“A）”，即使后面紧跟真正的 A./B.。
        // 题内已有明确 A./B. 选项时，①②等是题干中的编号陈述，不再切成选项。
        if (!allowNumericLabels && match.captured(1).isEmpty())
            continue;
        markers.append(match);
    }
    if (markers.isEmpty())
        return {};
    const QString lead = line.left(markers.first().capturedStart());
    QString normalizedLead = lead.trimmed();
    // Markdown 的无序列表会把纯选项行写成 “- A 选项”。这个短横线只是版式，
    // 不能被当成题干前缀；其他非空前缀仍按原规则保护，避免误切正文中的字母。
    if (normalizedLead == QStringLiteral("-") || normalizedLead == QStringLiteral("•"))
        normalizedLead.clear();
    const bool hasLeadText = !normalizedLead.isEmpty();
    // 单 marker 且前面有文字 → 视为题干，不当选项切。
    if (markers.size() == 1 && hasLeadText)
        return {};
    // ≥2 marker → 切分；若有 lead 文本则作为题干前缀回传。
    if (prefix && hasLeadText)
        *prefix = normalizedLead;
    QList<QPair<QString, QString>> result;
    for (int index = 0; index < markers.size(); ++index) {
        const auto& m = markers.at(index);
        // 从匹配里取出非空捕获组作为 label 原文，再归一化。
        QString rawLabel;
        for (int g = 1; g <= m.lastCapturedIndex(); ++g)
            if (!m.captured(g).isEmpty()) {
                rawLabel = m.captured(g);
                break;
            }
        const QString label = normalizeOptionLabel(rawLabel);
        if (label.isEmpty())
            continue;
        const int start = m.capturedEnd();
        const int end =
            index + 1 < markers.size() ? markers.at(index + 1).capturedStart() : line.size();
        const QString text = line.mid(start, end - start).trimmed();
        if (!text.isEmpty())
            result.append({label, text});
    }
    return result;
}

const QRegularExpression& trailingExplicitAnswerPattern() {
    static const QRegularExpression pattern(QStringLiteral(
        R"((?:【?\s*(?:参考|标准)?答案\s*】?|正确答案)\s*[:：]\s*([A-Fa-f]{1,6})\s*$)"));
    return pattern;
}

// 在完成题干合并和版式恢复后清理答案，同时映射 UTF-16 下划线坐标。
void blankBracketAnswer(MaterialTextWithDecorations* stem, const QRegularExpressionMatch& match) {
    const int start = match.capturedStart(2), end = match.capturedEnd(2);
    const auto remap = [start, end](int position) {
        if (position <= start) return position;
        if (position >= end) return position - (end - start) + 1;
        return start;
    };
    QJsonArray ranges;
    for (const auto& value : stem->underlines) {
        auto range = value.toObject();
        const int oldStart = range.value("start").toInt();
        const int newStart = remap(oldStart);
        const int newEnd = remap(oldStart + range.value("length").toInt());
        if (newEnd > newStart) {
            range.insert("start", newStart);
            range.insert("length", newEnd - newStart);
            ranges.append(range);
        }
    }
    stem->body.replace(start, end - start, QStringLiteral("　"));
    stem->underlines = ranges;
}

QString materialIdForQuestion(const QList<MaterialMarker>& materials, int questionLine,
                              int questionNumber) {
    QString id;
    for (const auto& material : materials) {
        if (material.line >= questionLine)
            break;
        if (material.firstQuestionLine > questionLine)
            continue;
        if (material.firstNumber > 0 &&
            (questionNumber < material.firstNumber || questionNumber > material.lastNumber))
            continue;
        id = material.id;
    }
    return id;
}

int nextMaterialLine(const QList<MaterialMarker>& materials, int afterLine, int fallback) {
    for (const auto& material : materials)
        if (material.line > afterLine && material.line < fallback)
            return material.line;
    return fallback;
}

QJsonObject parseQuestion(ExtractedDocument& document, const QList<SourceLine>& lines,
                          const QuestionAnchor& anchor, int blockEnd, const QString& stableId,
                          const QString& materialId, const QHash<int, QString>& answerKey,
                          const QHash<int, QString>& solutionKey,
                          QHash<QString, QByteArray>* generatedAssets, QString* reviewReason,
                          bool allowMultipleAnswers, bool insideGraphicalReasoningPart,
                          bool hasAnswerKey, bool* answerEvidenceDetected) {
    QStringList stemLines;
    QList<int> stemSourceIndices;
    const auto appendStem = [&stemLines, &stemSourceIndices](const QString& text, int sourceIndex) {
        const QString cleaned = text.trimmed();
        if (cleaned.isEmpty()) return;
        stemLines.append(cleaned);
        stemSourceIndices.append(sourceIndex);
    };
    QList<QPair<QString, QString>> options;
    QString rawAnswer;
    QStringList explicitAnswers;
    const auto recordAnswer = [&](const QString& value) {
        rawAnswer = value.trimmed();
        explicitAnswers.append(rawAnswer);
        if (answerEvidenceDetected && !rawAnswer.isEmpty())
            *answerEvidenceDetected = true;
    };
    QStringList solutionLines;
    bool inSolution = false;
    bool afterOptions = false;
    static const QRegularExpression explicitLetterOption(QStringLiteral(
        R"((?:^|\s)[A-Fa-f]\s*[\.．、:：\)）])"));
    const auto hasLetterOption = [&](const QString& text) {
        const auto brackets = bracketAnswerCandidates(text);
        auto matches = explicitLetterOption.globalMatch(text);
        while (matches.hasNext()) {
            const auto match = matches.next();
            bool inBracket = false;
            for (const auto& bracket : brackets)
                if (match.capturedStart() >= bracket.capturedStart() &&
                    match.capturedStart() < bracket.capturedEnd()) { inBracket = true; break; }
            if (!inBracket) return true;
        }
        return false;
    };
    bool hasExplicitLetterOptions = hasLetterOption(anchor.firstStemLine);
    for (int index = anchor.line + 1; index < blockEnd && !hasExplicitLetterOptions; ++index)
        hasExplicitLetterOptions = hasLetterOption(lines.at(index).text);
    const bool allowNumericOptionLabels = !hasExplicitLetterOptions;
    QString firstPrefix;
    if (!anchor.firstStemLine.isEmpty()) {
        QString firstLine = anchor.firstStemLine;
        const auto tail = trailingExplicitAnswerPattern().match(firstLine);
        if (tail.hasMatch()) {
            recordAnswer(tail.captured(1));
            firstLine = firstLine.left(tail.capturedStart()).trimmed();
        }
        // 题号行可能同行带选项：1. 题干 A.甲 B.乙…。先抽选项，剩余文本作为题干。
        const auto firstOptions =
            optionsOnLine(firstLine, &firstPrefix, allowNumericOptionLabels);
        if (!firstOptions.isEmpty()) {
            options += firstOptions;
            afterOptions = true;
        }
        // 题干前缀：优先用切选项后剩的前缀，否则用整行（移除尾随答案）。
        if (!firstPrefix.isEmpty())
            appendStem(firstPrefix, anchor.line);
        else {
            const auto answerMatch = inlineAnswerPattern().match(firstLine);
            if (answerMatch.hasMatch()) {
                recordAnswer(answerMatch.captured(1));
            } else if (firstOptions.isEmpty())
                appendStem(firstLine, anchor.line);
        }
    }

    for (int index = anchor.line + 1; index < blockEnd; ++index) {
        const QString line = lines.at(index).text.trimmed();
        if (line.isEmpty())
            continue;
        const auto answerMatch = inlineAnswerPattern().match(line);
        if (answerMatch.hasMatch()) {
            recordAnswer(answerMatch.captured(1));
            inSolution = false;
            continue;
        }
        const auto solutionMatch = solutionPattern().match(line);
        if (solutionMatch.hasMatch()) {
            inSolution = true;
            const QString first = solutionMatch.captured(1).trimmed();
            if (!first.isEmpty())
                solutionLines.append(first);
            continue;
        }
        if (inSolution) {
            solutionLines.append(line);
            continue;
        }
        // 只提前剥离显式“答案：A”。括号必须留到题干/选项分离之后再判断，
        // 否则会误删选项里的注释，或在冲突时提前丢失原文。
        QString lineForOptions = line;
        const auto cutMatch = trailingExplicitAnswerPattern().match(lineForOptions);
        if (cutMatch.hasMatch()) {
            lineForOptions = lineForOptions.left(cutMatch.capturedStart()).trimmed();
            recordAnswer(cutMatch.captured(1));
        }
        QString prefix;
        const auto parsedOptions =
            optionsOnLine(lineForOptions, &prefix, allowNumericOptionLabels);
        if (!parsedOptions.isEmpty()) {
            if (!afterOptions && !prefix.isEmpty()) appendStem(prefix, index);
            options += parsedOptions;
            afterOptions = true;
        } else if (afterOptions && !options.isEmpty()) {
            // 选项已出现后的续行：仍可能含尾随答案，已剥过；归入最后选项文本。
            options.last().second += QStringLiteral("\n") + lineForOptions;
        } else {
            appendStem(lineForOptions, index);
        }
    }
    auto decoratedStem = buildMaterialText(document, lines, stemSourceIndices, stemLines, false);
    QString bracketReviewReason;
    const auto bracketCandidates = bracketAnswerCandidates(decoratedStem.body);
    if (bracketCandidates.size() > 1) {
        bracketReviewReason = QStringLiteral("题干中有多处疑似括号答案，已保留原文；请确认答案及应清理的位置");
    } else if (bracketCandidates.size() == 1) {
        const auto& bracket = bracketCandidates.first();
        const QString candidate = normalizeAnswer(bracket.captured(2));
        QSet<QString> available;
        for (const auto& option : options) available.insert(option.first);
        bool validOptions = available.size() >= 2 && available.size() == options.size();
        for (const QChar letter : candidate)
            validOptions &= available.contains(QString(letter.toLower()));
        const bool multiple = allowMultipleAnswers || decoratedStem.body.contains(
            QRegularExpression(QStringLiteral("多选|多项选|不定项")));
        if (!validOptions) {
            bracketReviewReason = QStringLiteral("括号中的疑似答案无法对应完整且唯一的选项，已保留原文；请核对");
        } else if (candidate.size() > 1 && !multiple) {
            bracketReviewReason = QStringLiteral("括号包含多个答案字母，但题目未标注多选，已保留原文；请核对");
        } else {
            if (answerEvidenceDetected)
                *answerEvidenceDetected = true;
            bool conflict = false;
            for (const QString& explicitAnswer : explicitAnswers)
                conflict |= sortedAnswer(answerFromOptionText(explicitAnswer, options)) != sortedAnswer(candidate);
            if (answerKey.contains(anchor.number))
                conflict |= sortedAnswer(answerKey.value(anchor.number)) != sortedAnswer(candidate);
            if (conflict) {
                bracketReviewReason = QStringLiteral("括号答案与显式答案或答案表不一致，已保留原文；请确认正确答案");
            } else {
                rawAnswer = candidate;
                blankBracketAnswer(&decoratedStem, bracket);
            }
        }
    }
    stemLines = decoratedStem.body.split(QChar('\n'));
    QString answer = answerFromOptionText(rawAnswer, options);
    if (answerEvidenceDetected && answerKey.contains(anchor.number))
        *answerEvidenceDetected = true;
    if (answer.isEmpty())
        answer = answerKey.value(anchor.number);
    if (solutionLines.isEmpty() && solutionKey.contains(anchor.number))
        solutionLines.append(solutionKey.value(anchor.number));

    const QString visualText = stemLines.join(QChar('\n'));
    const bool hasLayoutCue = visualText.contains(QRegularExpression(QStringLiteral("划线|画线|横线|下划线|空白处")));
    if (hasLayoutCue && document.extractionBackend.startsWith(QStringLiteral("mineru"))) {
        recoverTrailingMineruBlank(&decoratedStem);
        stemLines = decoratedStem.body.split(QChar('\n'));
    }
    // 判断题：题干以空括号“（ ）”结尾且没有列 A/B 选项。合成“正确/不正确”两个
    // 选项走现有 true_false 通道（schema、校验器、前端 RadioButton 全兼容），答案
    // 由 对/错/√/× 归一化到合成选项。rawAnswer 优先，其次全局答案区。
    bool forcedTrueFalse = false;
    if (options.isEmpty() && hasBlankJudgmentBrackets(visualText)) {
        // rawAnswer 是题内原始文本（如“对/√”）；answerKey 来自全局答案区，判断题
        // 已在 globalAnswers 归一化为合成选项 A/B。两种来源都兼容。
        QString label = booleanAnswerLabel(rawAnswer);
        if (label.isEmpty()) {
            const QString keyed = answerKey.value(anchor.number);
            if (keyed == QStringLiteral("A") || keyed == QStringLiteral("B"))
                label = keyed;
            else
                label = booleanAnswerLabel(keyed);
        }
        options.append({QStringLiteral("a"), QStringLiteral("正确")});
        options.append({QStringLiteral("b"), QStringLiteral("不正确")});
        answer = label; // 空表示未识别到判断答案，交由后续复核逻辑处理。
        forcedTrueFalse = true;
    }
    const int sourcePage = anchor.line < lines.size() ? lines.at(anchor.line).page : 0;
    const bool hasVisualOptionLabels = sourcePage > 0 &&
        optionRowForQuestion(document, sourcePage, anchor.number).size() == 4;
    // 只有未能从文字层拆出至少两个选项时，才允许整页视觉上下文参与回退。否则
    // “如图/表”等普通题干会把整页试卷误挂到题干下；资料题尤其会把整段材料重复
    // 显示成图片。
    const bool hasVisualContext = options.size() < 2 && sourcePage > 0 &&
        (visualText.contains(QStringLiteral("图")) || visualText.contains(QStringLiteral("表")) ||
         visualText.contains(QStringLiteral("统计")) || visualText.contains(QStringLiteral("问号")));
    // 图题也可以拥有完全可靠的文字选项。此前为避免普通“图/表”题误挂整页图，
    // 把这类题排除在视觉附件外，因而漏掉了“如图所示”的题干插图。显式的指图
    // 表述是更强的证据，应单独触发，并使用题干插图裁切模式而非选项图片模式。
    const bool hasStemIllustrationCue = sourcePage > 0 &&
        visualText.contains(QRegularExpression(
            QStringLiteral("如图(?:所示)?|如下图|下图所示|图示|图中|示意图|见图")));
    const bool needsVisualOptions = options.size() < 2 &&
        (hasVisualOptionLabels || hasVisualContext);
    QHash<QString, QJsonObject> optionImages;
    bool attachStemImage = hasVisualContext || hasStemIllustrationCue;
    if (needsVisualOptions) {
        // PDF 文字层给出的 A/B/C/D 坐标并不总和页面绘制坐标一致。第 76 题
        // 就会把题号和第 77 题文字误裁成四张选项图。只要选项正文无法可靠
        // 提取，就统一保留“本题到下一题之前”的完整原卷题图；宁可让四个
        // 作答按钮只显示图 A/B/C/D，也不能生成看似精细但内容错误的小图。
        attachStemImage = true;
        options.clear();
        for (const QChar label : QStringLiteral("abcd")) {
            const QString id(label);
            options.append({id, QStringLiteral("图%1").arg(label.toUpper())});
        }
        // MinerU 给出的独立 A/B/C/D span 坐标已经过适配器归一化。四个标签必须
        // 同行、顺序完整且原 PDF 页面可渲染时，才把每个选项裁成独立图片；任何
        // 条件不满足都保持整题截图 + 硬复核，绝不猜测缺失选项的边界。
        if (document.extractionBackend.startsWith(QStringLiteral("mineru"))) {
            ensurePdfPageImages(&document, {sourcePage});
            optionImages = extractOptionImages(
                document, sourcePage, anchor.number, generatedAssets);
            if (optionImages.size() == 4)
                attachStemImage = hasStemIllustrationCue;
            else
                optionImages.clear();
        }
        // 文字层只剩“A、 B、 C、 D、”时，这一行是图形/公式选项的标签，不是
        // 题干。作答按钮会显示标签，结构化题干里不应再重复一遍空字母。
        static const QRegularExpression bareVisualLabels(QStringLiteral(
            R"(^\s*(?:[A-D]\s*[、.．]?\s*){2,}$)"),
            QRegularExpression::CaseInsensitiveOption);
        for (int index = stemLines.size() - 1; index >= 0; --index)
            if (bareVisualLabels.match(stemLines.at(index)).hasMatch())
                stemLines.removeAt(index);
    }

    QJsonArray jsonOptions;
    QSet<QString> optionIds;
    bool repeatedOptions = false;
    for (const auto& option : options) {
        if (optionIds.contains(option.first)) repeatedOptions = true;
        optionIds.insert(option.first);
        QJsonObject jsonOption{{"id", option.first}, {"text", option.second}};
        if (optionImages.contains(option.first))
            jsonOption.insert("image", optionImages.value(option.first));
        jsonOptions.append(jsonOption);
    }
    QJsonArray answerIds;
    for (const QChar choice : answer) {
        const QString id(choice.toLower());
        if (optionIds.contains(id))
            answerIds.append(id);
    }

    QStringList reasons;
    if (!bracketReviewReason.isEmpty()) reasons.append(bracketReviewReason);
    int samePageAnchors = 0;
    for (const auto& sourceAnchor : document.questionAnchors.value(sourcePage))
        if (sourceAnchor.text.toInt() == anchor.number) ++samePageAnchors;
    if (attachStemImage && samePageAnchors > 1)
        reasons.append(QStringLiteral("同页原题号重复，无法唯一定位题图；请对照原卷手动裁切并确认，不能共用第一题的截图"));
    if (repeatedOptions)
        reasons.append(QStringLiteral("重复选项标签，可能缺失题号或跨题合并；请对照原卷拆分，不能直接采用"));
    if (stemLines.join(QChar('\n')).trimmed().isEmpty())
        reasons.append(QStringLiteral("缺少题干"));
    if (jsonOptions.size() < 2)
        reasons.append(QStringLiteral("未识别到至少两个完整选项"));
    if (hasAnswerKey) {
        if (answer.isEmpty())
            reasons.append(rawAnswer.isEmpty() ? QStringLiteral("未识别到答案")
                                               : QStringLiteral("答案文本无法唯一匹配选项"));
        else if (answerIds.size() != answer.size())
            reasons.append(QStringLiteral("答案引用了不存在的选项"));
    }
    // 多答案题型信号来源有二：题干显式标注，或所属 section 大标题（真题常只在大
    // 标题标一次，经 allowMultipleAnswers 传入）。二者任一命中即允许多个正确答案。
    // 其中“严格多选”（题干明确写“多选/多项选”且非不定项）才要求答案≥2 个；不定项
    // 与 section 传播允许单答案（不定项少选也得分），避免误报“多选答案少于两个”。
    const QString stemJoined = stemLines.join(QChar('\n'));
    const bool indefiniteInStem = stemJoined.contains(QStringLiteral("不定项"));
    const bool strictMultipleInStem =
        stemJoined.contains(QRegularExpression(QStringLiteral("多选|多项选")));
    const bool markedMultipleInStem = strictMultipleInStem || indefiniteInStem;
    const bool allowsMultiple = markedMultipleInStem || allowMultipleAnswers;
    if (hasAnswerKey && answerIds.size() > 1 && !allowsMultiple)
        reasons.append(QStringLiteral("题干未标注多选题，却识别到多个答案"));
    if (hasAnswerKey && strictMultipleInStem && !indefiniteInStem && answerIds.size() < 2)
        reasons.append(QStringLiteral("多选题答案少于两个选项"));
    QString type = QStringLiteral("single_choice");
    // 判断题优先判定：空括号“（ ）”合成 正确/不正确 两个选项的题，直接定型，
    // 不依赖选项文本里是否同时出现“正确/错误”（“不正确”不含“错误”，旧检测会漏）。
    if (forcedTrueFalse)
        type = QStringLiteral("true_false");
    else if (allowsMultiple && (answerIds.size() > 1 || !hasAnswerKey))
        type = QStringLiteral("multiple_choice");
    else if (jsonOptions.size() == 2) {
        const QString first = jsonOptions.at(0).toObject().value("text").toString();
        const QString second = jsonOptions.at(1).toObject().value("text").toString();
        if (((first.contains(QStringLiteral("正确")) && second.contains(QStringLiteral("错误"))) ||
             (first.contains(QStringLiteral("错误")) && second.contains(QStringLiteral("正确")))) ||
            ((first == QStringLiteral("对") && second == QStringLiteral("错")) ||
             (first == QStringLiteral("错") && second == QStringLiteral("对"))))
            type = QStringLiteral("true_false");
    }
    QJsonObject source{{"document", QFileInfo(document.sourcePath).fileName()}};
    if (anchor.line < lines.size() && lines.at(anchor.line).page > 0)
        source.insert("page", lines.at(anchor.line).page);
    source.insert("questionNumber", anchor.number);
    source.insert("questionLabel", QString::number(anchor.number));
    if (!document.sectionId.isEmpty()) {
        source.insert("sectionId", document.sectionId);
        source.insert("sectionTitle", document.sectionTitle);
    }
    QJsonObject question{{"id", stableId},
                         {"catalogId", "generated"},
                         {"type", type},
                         {"stem", stemLines.join(QChar('\n')).trimmed()},
                         {"options", jsonOptions},
                         {"source", source}};
    if (!decoratedStem.underlines.isEmpty() && question.value("stem").toString() == decoratedStem.body)
        question.insert("stemUnderlines", decoratedStem.underlines);
    if (hasAnswerKey) {
        question.insert("answer", QJsonObject{{"optionIds", answerIds}});
        question.insert("solution", solutionLines.join(QChar('\n')).trimmed());
    }
    if (!materialId.isEmpty())
        question.insert("materialId", materialId);
    // 图形推理、统计资料和明确提到图/表的题保留原卷可视内容。无可靠选项文字
    // 锚点时，使用“本题到下一题前”的裁切图，而不是把整页试卷挂到题干下。
    if (attachStemImage) {
        // 纯文字题不会预先渲染 PDF 页。只有这里已经确定需要题图、图表或图片
        // 选项时才按需载入对应页，避免整理一本普通文字卷时无谓地栅格化整卷。
        if (sourcePage > 0)
            ensurePdfPageImages(&document, {sourcePage});
        QStringList stemSourceLines;
        for (const int sourceIndex : stemSourceIndices)
            if (sourceIndex >= 0 && sourceIndex < lines.size())
                stemSourceLines.append(lines.at(sourceIndex).text);
        const QJsonObject visualImage = extractQuestionVisualImage(
            document, sourcePage, anchor.number, stemSourceLines, hasStemIllustrationCue,
            generatedAssets);
        if (!visualImage.isEmpty())
            question.insert("stemImage", visualImage);
        else if (!QImage::fromData(document.pageImages.value(sourcePage), "PNG").isNull()) {
            // 只有测试夹具或损坏 PDF 缺少题号坐标时才保留历史整页回退；真实
            // PDF 一旦有题号锚点必走上面的局部裁切，不能把相邻题目带进来。
            const QString path = QStringLiteral("assets/%1-p%2.png")
                .arg(assetBaseName(document.sourcePath)).arg(sourcePage);
            question.insert("stemImage", visualAssetDescriptor(document, sourcePage,
                QRectF(0.0, 0.0, 1.0, 1.0), path, QStringLiteral("原卷图表")));
        }
    }
    if (!question.contains("stemImage") && (repeatedOptions ||
        (hasLayoutCue && decoratedStem.underlines.isEmpty() && !decoratedStem.body.contains(QStringLiteral("〔填空〕"))))) {
        const auto reference = captureSourceBlock(&document, lines, anchor.line, blockEnd, stableId, generatedAssets);
        if (!reference.isEmpty()) question.insert("stemImage", reference);
    }
    // 高危名单：结构可能完全合法，但内容正确性规则原理上验证不了，命中即强制
    // 复核（riskLevel=soft），不因为规则没报错就免检放行。跨页题的信号计算依赖
    // 相邻题的版面位置，留待后续批量审计阶段（RuleBasedGenerationAudit）补充。
    QStringList softSignals;
    if (insideGraphicalReasoningPart)
        softSignals.append(QStringLiteral("material-type:图形推理"));
    // “划线词语”“填入空白处”题的关键信息常是 PDF 文字层拿不到的下划线、
    // 空白横线或嵌入小图。即使结构化文本完整，也要提示复核者查看共享材料的
    // 原卷版式截图，不能把文本提取结果当作视觉信息完整。
    if (!materialId.isEmpty() && stemJoined.contains(QRegularExpression(
            QStringLiteral("划线|填入.*(?:空白|横线|下划线)|空白处"))))
        softSignals.append(QStringLiteral("material-layout:underline-or-blank"));
    if (materialId.isEmpty() && stemJoined.contains(QRegularExpression(
            QStringLiteral("划线|画线|横线|下划线|空白处"))))
        softSignals.append(QStringLiteral("stem-layout:underline-or-blank"));
    bool hasImageOption = false;
    for (const auto& optionValue : jsonOptions)
        if (optionValue.toObject().contains("image")) { hasImageOption = true; break; }
    if (question.contains("stemImage") || hasImageOption)
        softSignals.append(QStringLiteral("image-content"));
    if (document.usedOcr)
        softSignals.append(QStringLiteral("ocr-source"));
    if (!reasons.isEmpty()) {
        *reviewReason = reasons.join(QStringLiteral("；"));
        QJsonObject review{{"needsReview", true}, {"confidence", 0.25},
                           {"reason", *reviewReason}, {"riskLevel", "hard"}};
        if (!softSignals.isEmpty()) review.insert("signals", QJsonArray::fromStringList(softSignals));
        question.insert("review", review);
    } else if (!softSignals.isEmpty()) {
        // 规则没有报错，但命中高危名单：结构进入正常候选（questions），复核信息
        // 仍然标注 needsReview，交由制作器复核页按 riskLevel=soft 展示和批量确认，
        // 而不是像 hard 失败那样打回 needsReviewQuestions 数组。
        question.insert("review", QJsonObject{{"needsReview", true},
                                              {"confidence", document.usedOcr ? 0.6 : 0.7},
                                              {"reason", hasLayoutCue
                                                  ? QStringLiteral("请对照原卷核对划线词句、填空数量和位置；必要时可手动标记下划线")
                                                  : QStringLiteral("规则无法验证图表/图形内容的正确性，请核对原图")},
                                              {"riskLevel", "soft"},
                                              {"signals", QJsonArray::fromStringList(softSignals)}});
    } else {
        question.insert("review", QJsonObject{{"needsReview", false},
                                              {"confidence", document.usedOcr ? 0.75 : 0.95}});
    }
    return question;
}

// 从规则生成器的稳定 id（"r<doc>-q<number>-<ordinal>"）里取回原始题号，用于
// 题号连续性等批量统计；不依赖单独持久化原始题号字段。
int originalQuestionNumber(const QJsonObject& question) {
    static const QRegularExpression pattern(QStringLiteral(R"(-q(\d+)-)"));
    const auto match = pattern.match(question.value("id").toString());
    return match.hasMatch() ? match.captured(1).toInt() : -1;
}

// 把一个复核信号追加到题目的 review.signals，并按需把该题标记为待复核
// （riskLevel 不覆盖已有的 hard，只在题目当前完全常规或已是 soft 时设为 soft）。
// 统计信号只能加严，不能把已经 hard 失败的题目降级为看似较轻的 soft。
QJsonObject withReviewSignal(QJsonObject question, const QString& signal, const QString& reason) {
    QJsonObject review = question.value("review").toObject();
    QJsonArray signalList = review.value("signals").toArray();
    bool alreadyPresent = false;
    for (const auto& value : signalList)
        if (value.toString() == signal) { alreadyPresent = true; break; }
    if (!alreadyPresent) signalList.append(signal);
    review.insert("signals", signalList);
    review.insert("needsReview", true);
    if (review.value("riskLevel").toString() != QStringLiteral("hard"))
        review.insert("riskLevel", QStringLiteral("soft"));
    if (!review.contains("confidence")) review.insert("confidence", 0.6);
    QString existingReason = review.value("reason").toString();
    if (!existingReason.contains(reason))
        review.insert("reason", existingReason.isEmpty() ? reason
            : existingReason + QStringLiteral("；") + reason);
    question.insert("review", review);
    return question;
}

// 批量统计审计：单题看不出问题，只有跟“这批题应该是什么样子”做比对才能发现。
// 只在已通过单题结构校验的 result.questions 里运行，按来源文档分组；样本量
// 太小时统计意义不大，不产生信号。命中的题目追加 signals，不移出 questions
// 数组（riskLevel=soft 由复核页决定是否强制人工/AI 复核，而不是直接打回）。
void applyRuleBasedGenerationAudit(RuleBasedGenerationResult* result, bool hasAnswerKey) {
    QHash<QString, QList<int>> indicesByDocument;
    for (qsizetype index = 0; index < result->questions.size(); ++index) {
        const QJsonObject question = result->questions.at(index).toObject();
        const QString document = question.value("source").toObject().value("document").toString();
        const QString section = question.value("source").toObject().value("sectionId").toString();
        indicesByDocument[document + QChar(0x1f) + section].append(int(index));
    }

    for (auto it = indicesByDocument.constBegin(); it != indicesByDocument.constEnd(); ++it) {
        const QList<int>& indices = it.value();
        if (indices.size() < 4)
            continue; // 样本太小，众数/分布统计没有意义，避免对小题库误报。

        // 1) 题号连续性：源文档能提取出题号时，检测生成结果覆盖的题号区间是否
        // 有缺口。缺口本身不知道是哪道题的问题，因此作为整批 warning 而不是
        // 挂在某道具体题目上。
        QList<int> numbers;
        for (int index : indices) {
            const int number = originalQuestionNumber(result->questions.at(index).toObject());
            if (number > 0) numbers.append(number);
        }
        if (numbers.size() == indices.size()) {
            // 待复核的题仍然已识别，不能再次报为漏题。
            for (const auto& value : result->needsReviewQuestions) {
                const auto source = value.toObject().value("source").toObject();
                if (source.value("document").toString() + QChar(0x1f) +
                    source.value("sectionId").toString() == it.key())
                    numbers.append(source.value("questionNumber").toInt());
            }
            std::sort(numbers.begin(), numbers.end());
            QStringList missing;
            for (int expected = numbers.first(); expected <= numbers.last(); ++expected)
                if (!numbers.contains(expected)) missing.append(QString::number(expected));
            if (!missing.isEmpty())
                result->warnings.append(QStringLiteral("%1：题号 %2—%3 之间缺少第 %4 题，"
                    "请核对是否有题目未被识别").arg(QString(it.key()).replace(QChar(0x1f), QStringLiteral(" · "))).arg(numbers.first())
                    .arg(numbers.last()).arg(missing.join(QStringLiteral("、"))));
        }

        // 2) 选项数量分布：同一份文档的选项数应该基本一致；偏离众数的题目
        // 可能被截断或多算了一行。
        QHash<int, int> optionCountFrequency;
        for (int index : indices)
            ++optionCountFrequency[result->questions.at(index).toObject()
                .value("options").toArray().size()];
        int modeOptionCount = -1, modeFrequency = 0;
        for (auto freqIt = optionCountFrequency.constBegin(); freqIt != optionCountFrequency.constEnd();
             ++freqIt)
            if (freqIt.value() > modeFrequency) { modeFrequency = freqIt.value(); modeOptionCount = freqIt.key(); }
        if (modeOptionCount > 0 && modeFrequency >= indices.size() * 3 / 4) {
            for (int index : indices) {
                const QJsonObject question = result->questions.at(index).toObject();
                if (question.value("options").toArray().size() != modeOptionCount)
                    result->questions[index] = withReviewSignal(question,
                        QStringLiteral("option-count-outlier"),
                        QStringLiteral("本题选项数与同批次题目的众数（%1 个）不一致，"
                                       "可能选项被截断或多算").arg(modeOptionCount));
            }
        }

        // 3) 答案分布：单一答案字母占比异常高，通常指向选项/答案系统性错位，
        // 而不是巧合。只在样本量足够大时启用，避免小题库正常撞车触发误报。
        if (hasAnswerKey && indices.size() >= 8) {
            QHash<QString, int> answerFrequency;
            for (int index : indices) {
                const QJsonArray answerIds = result->questions.at(index).toObject()
                    .value("answer").toObject().value("optionIds").toArray();
                if (answerIds.size() == 1) ++answerFrequency[answerIds.first().toString()];
            }
            for (auto freqIt = answerFrequency.constBegin(); freqIt != answerFrequency.constEnd(); ++freqIt) {
                if (freqIt.value() * 5 < indices.size() * 3) continue; // 占比阈值 60%。
                for (int index : indices) {
                    const QJsonObject question = result->questions.at(index).toObject();
                    const QJsonArray answerIds =
                        question.value("answer").toObject().value("optionIds").toArray();
                    if (answerIds.size() == 1 && answerIds.first().toString() == freqIt.key())
                        result->questions[index] = withReviewSignal(question,
                            QStringLiteral("answer-distribution-skew"),
                            QStringLiteral("本题答案 %1 在同批次题目中占比过高（%2/%3），"
                                           "请核对选项与答案是否对齐").arg(freqIt.key())
                                .arg(freqIt.value()).arg(indices.size()));
                }
            }
        }

    }
}

} // namespace

RuleBasedGenerationResult
RuleBasedBankGenerator::generate(const QList<ExtractedDocument>& documents, bool hasAnswerKey,
                                 const RuleGenerationProgressCallback& progress) const {
    RuleBasedGenerationResult result;
    result.hasAnswerKey = hasAnswerKey;
    int documentOrdinal = 0;
    QList<ExtractedDocument> scopedDocuments;
    for (const auto& sourceDocument : documents) {
        ExtractedDocument document = sourceDocument;
        // 正式的本地 PDF / MinerU 适配器已在提取阶段执行；这里保留一次幂等
        // 清理，使测试夹具和其它 ExtractedDocument 生产者同样遵守边栏契约。
        stripRepeatedPageFurniture(&document);
        QList<ExtractedDocument> sections = splitBookletSections(document);
        QStringList sectionTitles;
        for (const ExtractedDocument& section : sections)
            if (!section.sectionTitle.isEmpty())
                sectionTitles.append(section.sectionTitle);
        for (ExtractedDocument& section : sections) {
            if (!document.companionAnswerText.trimmed().isEmpty()) {
                const QString companion = companionAnswerTextForSection(
                    document.companionAnswerText, section.sectionTitle, sectionTitles);
                section.plainText += QStringLiteral("\n\n答案及解析\n") + companion;
                section.companionAnswerText.clear();
            }
            scopedDocuments.append(section);
        }
    }
    int processedQuestions = 0;
    int sectionOrdinal = 0;
    for (const auto& sourceDocument : scopedDocuments) {
        // 下划线装饰是按需补充的版面元数据，生成时只影响当前文档的副本。
        ExtractedDocument document = sourceDocument;
        ++sectionOrdinal;
        ++documentOrdinal;
        QList<SourceLine> lines = sourceLines(document);
        normalizeTrailingQuestionNumberLayout(&lines);
        const QSet<int> inferredQuestionLines =
            recoverSingleMissingQuestionNumbers(&document, &lines);

        // 收集所有答案区头行号。整体前后分开的文件只有一个，阶段分组的文件
        // 会有多个（每个阶段一组题+一组答案）。答案区头本身不作为题目终点，
        // 但它界定了“从这里开始进入答案文本”。
        // 排版紧凑的真题会把答案串直接接在标题后（“【参考答案】CAACD…”），
        // 这类行同样是答案区起点，只是扫描时必须连它自己一起读。
        QList<int> answerSections;
        QSet<int> inlineAnswerSections;
        for (int index = 0; index < lines.size(); ++index) {
            const QString& text = lines.at(index).text;
            if (isAnswerSectionHeader(text)) {
                answerSections.append(index);
            } else if (isInlineAnswerSectionHeader(text)) {
                answerSections.append(index);
                inlineAnswerSections.insert(index);
            }
        }
        const int firstAnswerSection = answerSections.isEmpty() ? -1 : answerSections.first();
        // contentEnd 用于框定材料扫描范围与题目块终点：在没有任何答案区头时，整
        // 篇都是题目；否则题目区止于第一个答案区头。阶段二、三的题目区在其各自
        // 的答案区头之后，会经由 blockEnd 被正确切分。
        const int contentEnd = firstAnswerSection >= 0 ? firstAnswerSection : lines.size();

        // 材料头可能出现在任意阶段，整体扫描而非只扫到第一个答案区头前。
        QList<MaterialMarker> materialMarkers;
        QSet<int> materialHeaderLinesClaimedBySectionMarker;
        int materialOrdinal = 0;
        static const QRegularExpression rangePattern(
            QStringLiteral(R"((\d{1,4})\s*(?:[-—~～]|至|到)\s*(\d{1,4})\s*题)"));

        // 题号锚点扫描：覆盖阶段分组里出现在答案区头之后的题目。答案区头行不算
        // 锚点；同时排除“1.A”“2.B”这类答案汇总行（题号后紧跟答案字母）。
        // 关键规则：最后一个答案区头之后的候选锚点都属于“末尾答案区”内的解析
        // 文本（如“1. 某选项是第一个”），一律剔除；位于前序答案区头之间的候选
        // 锚点才是阶段二的题目（前一个答案区已结束、下一个答案区未开始）。
        // 一行可含多组答案对（“1.A 2.B 3.C”），整行都是答案对时都不算题号锚点。
        static const QRegularExpression answerRecordLine(QStringLiteral(
            R"(^\s*(?:(?:第\s*)?\d{1,4}\s*(?:题|[\.．、:：\)）])\s*(?:【?答案】?\s*[:：]?)?\s*[A-Fa-f]{1,6}\s*)+\s*$)"));
        const int lastAnswerSection = answerSections.isEmpty() ? -1 : answerSections.last();
        QList<QuestionAnchor> anchors;
        for (int index = 0; index < lines.size(); ++index) {
            if (isAnswerSectionHeader(lines.at(index).text))
                continue;
            const QString text = lines.at(index).text;
            if (answerRecordLine.match(text.trimmed()).hasMatch())
                continue;
            // 落在最后一个答案区头之后 → 属于末尾答案区的解析文本，剔除。
            if (lastAnswerSection >= 0 && index > lastAnswerSection)
                continue;
            const auto match = questionPattern().match(text);
            if (!match.hasMatch())
                continue;
            const int number = match.captured(1).toInt();
            if (number <= 0)
                continue;
            anchors.append({index, number, match.captured(2).trimmed(),
                            inferredQuestionLines.contains(index)});
        }

        const auto reportProgress = [&](int questionIndex) {
            if (!progress)
                return;
            RuleGenerationProgress snapshot;
            snapshot.documentName = QFileInfo(document.sourcePath).fileName();
            snapshot.sectionTitle = document.sectionTitle;
            snapshot.sectionIndex = sectionOrdinal;
            snapshot.sectionCount = scopedDocuments.size();
            snapshot.questionIndex = questionIndex;
            snapshot.questionCount = anchors.size();
            snapshot.processedQuestions = processedQuestions;
            snapshot.acceptedQuestions = result.questions.size();
            snapshot.reviewQuestions = result.needsReviewQuestions.size();
            snapshot.pageImageCount = document.pageImages.size();
            snapshot.pageImageBytes = document.pageImages.byteSize();
            snapshot.reviewAssetCount = result.reviewAssets.size();
            snapshot.reviewAssetBytes = assetBytes(result.reviewAssets);
            progress(snapshot);
        };
        reportProgress(0);

        // 材料扫描：材料头与“根据材料回答N-M题”的范围头一起出现，范围头可能与
        // 材料头同段。这里复用既有的范围正则与归属逻辑。
        for (int line = 0; line < lines.size(); ++line) {
            int contentLine = line;
            if (isMaterialSectionMarker(lines.at(line).text)) {
                // “（二）”常单独占行；它本身不该粘到上一道题的 D 选项。只有
                // 后面的首个非空行确为资料头时，才把它作为下一份材料的起点。
                int next = line + 1;
                while (next < lines.size() && lines.at(next).text.trimmed().isEmpty()) ++next;
                if (next >= lines.size() || !isMaterialHeader(lines.at(next).text))
                    continue;
                contentLine = next;
                materialHeaderLinesClaimedBySectionMarker.insert(contentLine);
            } else if (!isMaterialHeader(lines.at(line).text)) {
                continue;
            } else if (materialHeaderLinesClaimedBySectionMarker.contains(line)) {
                continue;
            }
            int firstQuestionLine = -1;
            for (const auto& anchor : anchors) {
                if (anchor.line > line) {
                    firstQuestionLine = anchor.line;
                    break;
                }
            }
            if (firstQuestionLine < 0)
                continue;
            int firstNumber = 0;
            int lastNumber = 0;
            QString rangeText;
            for (int cursor = line; cursor < firstQuestionLine; ++cursor)
                rangeText += lines.at(cursor).text + QChar('\n');
            const auto range = rangePattern.match(rangeText);
            if (range.hasMatch()) {
                firstNumber = range.captured(1).toInt();
                lastNumber = range.captured(2).toInt();
                if (firstNumber > lastNumber)
                    qSwap(firstNumber, lastNumber);
            }
            // 阅读理解的材料常没有“完成 N-M 题”字样。遇到下一大题标题时，将
            // 归属截到标题前最后一道题，避免 76 之类独立数量题被误挂到前文。
            if (firstNumber == 0) {
                int sectionLine = -1;
                for (int cursor = firstQuestionLine + 1; cursor < lines.size(); ++cursor)
                    if (isTopLevelSectionHeading(lines.at(cursor).text)) {
                        sectionLine = cursor;
                        break;
                    }
                if (sectionLine >= 0) {
                    for (const auto& anchor : anchors)
                        if (anchor.line >= firstQuestionLine && anchor.line < sectionLine) {
                            if (firstNumber == 0) firstNumber = anchor.number;
                            lastNumber = anchor.number;
                        }
                }
            }
            const QString id =
                QStringLiteral("r%1-m%2").arg(documentOrdinal).arg(++materialOrdinal);
            materialMarkers.append({line, contentLine, firstQuestionLine, firstNumber, lastNumber, id});
        }

        // “划线/横线/标注/标记”小题才会触发像素级检测；找到小题后只向上回溯
        // 它所属共享材料的正文行。这样不会再整卷逐字符扫描，也不会把无关页面
        // 的装饰误收进材料文本。
        QHash<int, QStringList> underlineCandidateLinesByPage;
        for (int i = 0; i < anchors.size(); ++i) {
            const int start = anchors.at(i).line;
            const int end = i + 1 < anchors.size() ? anchors.at(i + 1).line : contentEnd;
            bool layoutCue = false;
            for (int j = start; j < end; ++j)
                if (lines.at(j).text.contains(QRegularExpression(
                        QStringLiteral("划线|画线|横线|下划线|空白处")))) layoutCue = true;
            if (!layoutCue) continue;
            for (int j = start; j < end; ++j) {
                const auto& source = lines.at(j);
                if (j > start && !optionsOnLine(source.text, nullptr, false).isEmpty()) break;
                if (source.page > 0) underlineCandidateLinesByPage[source.page].append(source.text);
            }
        }
        for (int markerIndex = 0; markerIndex < materialMarkers.size(); ++markerIndex) {
            const MaterialMarker& marker = materialMarkers.at(markerIndex);
            const int materialEndLine = markerIndex + 1 < materialMarkers.size()
                ? materialMarkers.at(markerIndex + 1).line : lines.size();
            if (!materialQuestionMentionsUnderline(lines, anchors, marker, materialEndLine))
                continue;
            for (int cursor = marker.contentLine + 1;
                 cursor < marker.firstQuestionLine; ++cursor) {
                const SourceLine& source = lines.at(cursor);
                if (source.page > 0 && !source.text.trimmed().isEmpty())
                    underlineCandidateLinesByPage[source.page].append(source.text);
            }
        }
        detectPdfUnderlinesForCandidateLines(&document, underlineCandidateLinesByPage);

        for (const MaterialMarker& marker : materialMarkers) {
            const int line = marker.line;
            const int contentLine = marker.contentLine;
            const int firstQuestionLine = marker.firstQuestionLine;
            const QString& id = marker.id;
            QStringList body;
            QList<int> bodySourceIndices;
            for (int cursor = contentLine + 1; cursor < firstQuestionLine; ++cursor)
                if (!lines.at(cursor).text.trimmed().isEmpty()) {
                    body.append(lines.at(cursor).text);
                    bodySourceIndices.append(cursor);
                }
            if (body.isEmpty()) {
                body.append(lines.at(contentLine).text);
                bodySourceIndices.append(contentLine);
            }
            QJsonObject source{{"document", QFileInfo(document.sourcePath).fileName()}};
            if (lines.at(line).page > 0)
                source.insert("page", lines.at(line).page);
            const MaterialTextWithDecorations materialText =
                buildMaterialText(document, lines, bodySourceIndices, body);
            const QString materialBody = materialText.body;
            QJsonObject material{{"id", id}, {"catalogId", "generated"},
                                 {"title", lines.at(contentLine).text.left(200)},
                                 {"body", materialBody}, {"source", source}};
            if (!materialText.underlines.isEmpty())
                material.insert(QStringLiteral("underlines"), materialText.underlines);
            const int firstPage = lines.at(line).page;
            const int lastPage = lines.at(firstQuestionLine).page;
            // 对 PDF 阅读材料保存裁切后的原卷版式。文字层无法表示下划线样式、
            // 空白横线和嵌入式图片横线；这一层视觉附件确保它们不再丢失，同时
            // 只裁材料范围，绝不把整页试卷错挂到子题题干。
            const bool dataAnalysisMaterial = isInsideDataAnalysisPart(lines, line);
            QList<int> materialPages;
            for (int page = firstPage; page <= lastPage; ++page)
                if (page > 0) materialPages.append(page);
            ensurePdfPageImages(&document, materialPages);
            QJsonArray images = extractMaterialLayoutImages(
                document, firstPage, lastPage, lines.at(line).text,
                lines.at(firstQuestionLine).text, id,
                dataAnalysisMaterial, &result.assets);
            // 文本锚点不可用的旧式/扫描夹具仍保留原有整页视觉回退；正式 PDF
            // 优先走上面的裁切路径，避免把无关题目混进阅读材料。
            if (images.isEmpty() &&
                (lines.at(contentLine).text.contains(QStringLiteral("资料")) ||
                 lines.at(contentLine).text.contains(QStringLiteral("图")) ||
                 lines.at(contentLine).text.contains(QStringLiteral("表")))) {
                for (int page = firstPage; page <= lastPage; ++page) {
                    if (!document.pageImages.contains(page)) continue;
                    const QString path = QStringLiteral("assets/%1-p%2.png")
                        .arg(assetBaseName(document.sourcePath)).arg(page);
                    images.append(visualAssetDescriptor(document, page, QRectF(0.0, 0.0, 1.0, 1.0),
                        path, QStringLiteral("原卷资料图表")));
                    result.assets.insert(path, document.pageImages.value(page));
                }
            }
            if (!images.isEmpty()) material.insert("images", images);
            // 资料分析中题目文本通常很短且规则识别相对稳定，真正需要核对的是
            // 共享资料的图表/单位/版式。因此只把材料放入 soft 复核队列。
            if (dataAnalysisMaterial) {
                material.insert("review", QJsonObject{
                    {"needsReview", true}, {"confidence", 0.7},
                    {"reason", QStringLiteral("请核对资料分析材料的图表、单位和版式")},
                    {"riskLevel", "soft"},
                    {"signals", QJsonArray{QStringLiteral("material-type:资料分析")}}});
            }
            result.materials.append(material);
        }

        // 逐段解析答案区。每段的作用域到下一个题号锚点或下一个材料头为止，
        // 这样阶段二的题目不会被阶段一的答案区吞掉。
        QHash<int, QString> answers;
        QHash<int, QString> solutions;
        QSet<int> ambiguousAnswerNumbers;
        // 各答案区段按行序的原始答案记录（题号→答案）。无标题的章节型题库题号
        // 会重启，按题号的 map 把各章的答案互相冲突掉；兜底 pass 用这里的顺序
        // 把答案按位置绑回紧邻的题块。
        QList<QList<QPair<int, QString>>> sectionAnswerRecords;
        sectionAnswerRecords.reserve(answerSections.size());
        for (int sectionIndex = 0; sectionIndex < answerSections.size(); ++sectionIndex) {
            const int sectionStart = answerSections.at(sectionIndex);
            const int sectionEnd =
                answerSectionEnd(lines, sectionStart, anchors, materialMarkers);
            QList<QPair<int, QString>> orderedRecords;
            const auto segmentAnswers = globalAnswers(lines, sectionStart, sectionEnd,
                                                      inlineAnswerSections.contains(sectionStart),
                                                      &ambiguousAnswerNumbers, &orderedRecords);
            // “参考答案/答案解析”标题本身不是答案证据：扫描件或残缺资料可能只
            // 识别出标题。只有真正解析到题号+答案记录时才锁定为含答案语义，
            // 否则 Auto 应允许工作流重跑成无答案题库。
            if (!orderedRecords.isEmpty())
                result.answerEvidenceDetected = true;
            sectionAnswerRecords.append(orderedRecords);
            for (auto it = segmentAnswers.cbegin(); it != segmentAnswers.cend(); ++it)
                if (answers.contains(it.key()) && sortedAnswer(answers.value(it.key())) != sortedAnswer(it.value()))
                    ambiguousAnswerNumbers.insert(it.key());
                else answers.insert(it.key(), it.value());
            const auto segmentSolutions = globalSolutions(lines, sectionStart, sectionEnd);
            for (auto it = segmentSolutions.cbegin(); it != segmentSolutions.cend(); ++it)
                if (!solutions.contains(it.key()))
                    solutions.insert(it.key(), it.value());
        }
        for (int number : ambiguousAnswerNumbers) {
            answers.remove(number);
            solutions.remove(number);
        }

        // 兜底：章节标题没被识别（形态未知）导致题号重启时，按题号绑定会整体
        // 失败。答案区紧跟在题块后面，若该段的答案记录恰好是连续题号区间 [a..a+K-1]
        // （K>=2）、且紧邻其前的 K 个锚点也构成同一段区间、其中至少一个题号在全文
        // 重号，则按位置一一对应。证据不足时宁可保持 hard 复核——错位写入比不
        // 写入更有害。
        QHash<int, QString> positionalAnswersByLine;
        if (hasAnswerKey && !anchors.isEmpty()) {
            QHash<int, int> counts;
            for (const auto& anchor : anchors)
                ++counts[anchor.number];
            for (int sectionIndex = 0; sectionIndex < answerSections.size(); ++sectionIndex) {
                const int sectionStart = answerSections.at(sectionIndex);
                const QList<QPair<int, QString>>& records = sectionAnswerRecords.at(sectionIndex);
                if (records.size() < 2)
                    continue;
                // 本段答案区之前的所有题号锚点（后面的章节不影响本段）。
                QList<const QuestionAnchor*> before;
                for (const auto& anchor : anchors)
                    if (anchor.line < sectionStart)
                        before.append(&anchor);
                const int recordCount = records.size();
                if (before.size() < recordCount)
                    continue;
                const int firstNumber = records.first().first;
                bool consecutive = true;
                bool hasRepeated = false;
                for (int i = 0; i < recordCount && consecutive; ++i) {
                    if (records.at(i).first != firstNumber + i)
                        consecutive = false;
                    else if (counts.value(records.at(i).first, 0) > 1)
                        hasRepeated = true;
                }
                if (!consecutive || !hasRepeated)
                    continue;
                // 紧邻其前的 K 个锚点必须构成同样的连续区间。跨章同号产生的
                // “冲突”（1 对应 A 也对应 B）正是需要按位置绑定的原因，不是
                // 拒绝条件；真正的歧义（一张表覆盖两章）由下面的多段匹配检查
                // 拦截。
                bool anchorsMatch = true;
                for (int i = 0; i < recordCount && anchorsMatch; ++i)
                    if (before.at(int(before.size()) - recordCount + i)->number != firstNumber + i)
                        anchorsMatch = false;
                if (!anchorsMatch)
                    continue;
                // 整卷只有一个答案区、而前面存在多段同形题号区间时，无法判断
                // 这张表属于哪一章（如两章各 1-6 题、卷末只有一张 1-6 的表）；
                // 宁可放弃。分章各自带答案区时按“章节顺序、答案紧跟本章”绑定。
                if (answerSections.size() == 1) {
                    int matchingRuns = 0;
                    for (int start = 0; start + recordCount <= int(before.size()); ++start) {
                        bool run = true;
                        for (int i = 0; i < recordCount && run; ++i)
                            if (before.at(start + i)->number != firstNumber + i)
                                run = false;
                        if (run)
                            ++matchingRuns;
                    }
                    if (matchingRuns >= 2)
                        continue;
                }
                for (int i = 0; i < recordCount; ++i) {
                    const QuestionAnchor& anchor = *before.at(int(before.size()) - recordCount + i);
                    if (counts.value(anchor.number) <= 1)
                        continue; // 未重号的题已有按题号绑定的答案。
                    positionalAnswersByLine.insert(anchor.line, records.at(i).second);
                }
            }
        }

        // 同一类网页导出还会把“正确答案：C”集中排在题干之前，且每行没有题号。
        // 只有答案数量恰好等于识别题数时才按顺序配对，避免把普通解析中的答案词
        // 错绑到题目；已有显式题号答案始终优先。
        QStringList leadingAnswers;
        if (hasAnswerKey && !anchors.isEmpty()) {
            for (int line = 0; line < anchors.first().line; ++line) {
                const auto match = inlineAnswerPattern().match(lines.at(line).text);
                if (!match.hasMatch())
                    continue;
                const QString answer = normalizeAnswer(match.captured(1));
                if (!answer.isEmpty())
                    leadingAnswers.append(answer);
            }
            if (leadingAnswers.size() != anchors.size()) leadingAnswers.clear();
        }

        // 末页答案区常见的“【参考答案】CAACD DBABC…”整串排版：没有任何题号，
        // 只能按顺序对应题号锚点。与上面的“题干前集中答案”同一条安全约束——
        // 数量必须完全对得上才采用，且不覆盖已有的显式答案；对不上时宁可让题目
        // 进复核，也不产出错位的答案。
        //
        // 不能用 answers.isEmpty() 当守卫：其他规则可能已给某些题配上答案（比如
        // 把末行 “ABC” 当成最后一题的多选答案），此时数量恰好对上时仍应采用。
        if (hasAnswerKey && !anchors.isEmpty()) {
            int missingCount = 0;
            for (const auto& anchor : anchors)
                if (!answers.contains(anchor.number))
                    ++missingCount;
            if (missingCount > 0) {
                for (const int sectionStart : answerSections) {
                    const int sectionEnd =
                        answerSectionEnd(lines, sectionStart, anchors, materialMarkers);
                    const int limit = sectionEnd >= 0 ? sectionEnd : lines.size();
                    QStringList run;
                    for (int line = sectionStart; line < limit && line < lines.size(); ++line) {
                        const QStringList lineRun = contiguousAnswerRun(lines.at(line).text);
                        if (lineRun.isEmpty())
                            continue;
                        run.append(lineRun);
                    }
                    if (run.isEmpty())
                        continue;
                    // 连续答案串即使暂时无法安全对齐题号，也证明原文确有答案；
                    // 保持含答案复核，不能静默丢弃。
                    result.answerEvidenceDetected = true;

                    // 整卷题数与答案数相等是最强证据，直接顺序配对。
                    if (run.size() == anchors.size()) {
                        for (int index = 0; index < anchors.size(); ++index)
                            if (!answers.contains(anchors.at(index).number) &&
                                !ambiguousAnswerNumbers.contains(anchors.at(index).number))
                                answers.insert(anchors.at(index).number, run.at(index));
                        break;
                    }

                    // 真题的答案串常常只覆盖主体大题，卷末还会附带没有答案的练习
                    // （“读题圈圈”“速算练习”等）。此时整卷数量对不上，但答案串仍能
                    // 严格对应某一段连续题号。只在恰好存在唯一一段长度相符、且题号
                    // 严格连续递增的锚点区间时采用——多于一段就无法判断该绑哪段，
                    // 一律放弃，避免错位。
                    int matchStart = -1;
                    int matchCount = 0;
                    for (int start = 0; start + run.size() <= anchors.size(); ++start) {
                        bool contiguous = true;
                        for (int offset = 1; offset < run.size(); ++offset) {
                            if (anchors.at(start + offset).number !=
                                anchors.at(start + offset - 1).number + 1) {
                                contiguous = false;
                                break;
                            }
                        }
                        if (!contiguous)
                            continue;
                        // 紧邻的题号也必须断开，否则这段只是更长连续序列的一部分，
                        // 起点无法确定。
                        if (start > 0 &&
                            anchors.at(start).number == anchors.at(start - 1).number + 1)
                            continue;
                        const int end = start + static_cast<int>(run.size());
                        if (end < anchors.size() &&
                            anchors.at(end).number == anchors.at(end - 1).number + 1)
                            continue;
                        matchStart = start;
                        ++matchCount;
                    }
                    if (matchCount != 1)
                        continue;
                    for (int offset = 0; offset < run.size(); ++offset) {
                        const int number = anchors.at(matchStart + offset).number;
                        if (!answers.contains(number) &&
                            !ambiguousAnswerNumbers.contains(number))
                            answers.insert(number, run.at(offset));
                    }
                    break;
                }
            }
        }

        if (anchors.isEmpty()) {
            QString reason = QStringLiteral("没有识别到题号锚点");
            if (document.ocrSkippedPages > 0)
                reason = QStringLiteral("有 %1 页需要文字识别（OCR），但当前版本未启用，尚未读到可用题目。这不是题号格式问题；请使用带 OCR 的版本或先转换为带文字层的 PDF")
                    .arg(document.ocrSkippedPages);
            else if (document.ocrFailedPages > 0)
                reason = QStringLiteral("有 %1 页文字识别（OCR）失败，尚未读到可用题目。%2")
                    .arg(document.ocrFailedPages)
                    .arg(document.warnings.value(0));
            result.warnings.append(QStringLiteral("%1：%2")
                .arg(QFileInfo(document.sourcePath).fileName(), reason));
            continue;
        }

        // 收集所有大标题及其是否为多答案题型（“二、多项选择题/不定项”等）。真题常
        // 在 section 标题标一次，下面每道题干不再重复；据此给段内每道题传播“允许多
        // 答案”，避免多选/不定项整段被打回复核。普通大题标题（如“三、判断题”）会
        // 取消上一段的多答案属性。多选与不定项在作答结构上等价，统一映射 multiple_choice。
        QList<QPair<int, bool>> sectionHeadings; // 行号 → 是否多答案
        for (int index = 0; index < lines.size(); ++index) {
            const QString text = lines.at(index).text;
            if (isTopLevelSectionHeading(text) || isMultiAnswerSectionHeading(text))
                sectionHeadings.append({index, isMultiAnswerSectionHeading(text)});
        }

        QHash<int, int> numberCounts;
        for (const auto& anchor : anchors) ++numberCounts[anchor.number];
        QHash<int, int> numberOccurrences;
        int questionOrdinal = 0;
        for (int index = 0; index < anchors.size(); ++index) {
            const QuestionAnchor& anchor = anchors.at(index);
            int blockEnd = index + 1 < anchors.size() ? anchors.at(index + 1).line : lines.size();
            // 题目块不能越过任何答案区头：遇到答案区头说明题目区已结束。
            for (int section : answerSections)
                if (section > anchor.line && section < blockEnd) {
                    blockEnd = section;
                    break;
                }
            blockEnd = nextMaterialLine(materialMarkers, anchor.line, blockEnd);
            // 本题所属 section 是否为多答案题型：取题号行之前最近的一个大标题
            // 判定（例如“三、判断题”应取消上一段“二、多项选择”的多答案属性）。
            bool allowMultipleAnswers = false;
            for (const auto& heading : sectionHeadings)
                if (heading.first < anchor.line)
                    allowMultipleAnswers = heading.second;
                else
                    break;
            const QString id = QStringLiteral("r%1-q%2-%3")
                                   .arg(documentOrdinal)
                                   .arg(anchor.number)
                                   .arg(++questionOrdinal);
            const QString materialId =
                materialIdForQuestion(materialMarkers, anchor.line, anchor.number);
            auto questionAnswers = answers;
            auto questionSolutions = solutions;
            const bool repeatedNumber = numberCounts.value(anchor.number) > 1;
            if (repeatedNumber) {
                // 同号表项不能同时绑定到多道题（解析也不例外）。题内答案仍可用。
                questionAnswers.remove(anchor.number);
                questionSolutions.remove(anchor.number);
            }
            // 数量严格吻合的前置答案按锚点序号绑定，不写入按题号去重的 map。
            if (!leadingAnswers.isEmpty() && !questionAnswers.contains(anchor.number) &&
                !ambiguousAnswerNumbers.contains(anchor.number))
                questionAnswers.insert(anchor.number, leadingAnswers.at(index));
            // 无标题章节型题库的题号重启无法按题号绑定时，用兜底 pass 按答案区
            // 顺序对位的结果（只写进当前题的副本，不影响其它同号题）。
            const bool positionallyBound = positionalAnswersByLine.contains(anchor.line) &&
                !questionAnswers.contains(anchor.number);
            if (positionallyBound)
                questionAnswers.insert(anchor.number, positionalAnswersByLine.value(anchor.line));
            QString reviewReason;
            QJsonObject question = parseQuestion(document, lines, anchor, blockEnd, id, materialId,
                                                 questionAnswers, questionSolutions, &result.assets, &reviewReason,
                                                 allowMultipleAnswers,
                                                 isInsideGraphicalReasoningPart(lines, anchor.line),
                                                 hasAnswerKey, &result.answerEvidenceDetected);
            // 正式题图只在题目确实依赖图片时随题库发布；校对页则应始终能看到
            // 原卷。普通文字题在这里额外生成一张 review-only 裁图，避免复核者
            // 只能对照转写文本，也避免把数百张校对图塞进最终题库安装包。
            QJsonObject sourcePreview = question.value(QStringLiteral("stemImage")).toObject();
            if (sourcePreview.isEmpty()) {
                // 普通文字题只保存原卷页码与裁切框。过去这里会立即渲染并把每题
                // 一张 PNG 全部放进 reviewAssets，600 题题本会与整页缓存共同抬高
                // Win7 峰值。复核页选中题目时再按该描述符生成并缓存当前一张。
                sourcePreview = describeLazySourceBlock(
                    document, lines, anchor.line, blockEnd, id);
                if (sourcePreview.isEmpty())
                    sourcePreview = describeLazySourcePage(
                        document, lines.at(anchor.line).page, id);
                if (!sourcePreview.isEmpty())
                    sourcePreview.insert(QStringLiteral("reviewOnly"), true);
            }
            if (!sourcePreview.isEmpty()) {
                sourcePreview.insert(QStringLiteral("alt"), QStringLiteral("原卷题目"));
                result.reviewSourceImages.insert(id, sourcePreview);
            }
            if (anchor.inferredNumber) {
                const QString reason = QStringLiteral("原卷缺失题号，已根据相邻题号与完整选项补为第 %1 题；请核对题目边界")
                    .arg(anchor.number);
                question = withReviewSignal(question, QStringLiteral("question-number-inferred"), reason);
                reviewReason = reviewReason.isEmpty() ? reason
                                                      : reviewReason + QStringLiteral("；") + reason;
            }
            if (repeatedNumber) {
                auto source = question.value("source").toObject();
                source.insert("questionLabel", QStringLiteral("原第 %1 题 · 同号第 %2 处")
                    .arg(anchor.number).arg(++numberOccurrences[anchor.number]));
                question.insert("source", source);
                const bool answerResolved =
                    !question.value("answer").toObject().value("optionIds").toArray().isEmpty();
                if (hasAnswerKey && !answerResolved) {
                    const QString reason = QStringLiteral("原题号 %1 重复，不能仅按题号确定答案与解析的归属；请对照原卷确认本题答案")
                        .arg(anchor.number);
                    reviewReason = reviewReason.isEmpty() ? reason : reviewReason + QStringLiteral("；") + reason;
                    auto review = question.value("review").toObject();
                    review.insert("needsReview", true);
                    review.insert("riskLevel", "hard");
                    review.insert("reason", reviewReason);
                    question.insert("review", review);
                } else if (positionallyBound) {
                    // 按位置绑定的答案仍然依赖“题块与答案区紧邻且题号连续”的假设，
                    // 标记 soft 复核，不遮挡已有的 hard 信号。
                    question = withReviewSignal(question, QStringLiteral("duplicate-number-positional-answer"),
                        QStringLiteral("原题号 %1 重复，答案已按本章答案区顺序与本题对应，请核对")
                            .arg(anchor.number));
                }
            }
            if (hasAnswerKey && ambiguousAnswerNumbers.contains(anchor.number) &&
                question.value("answer").toObject().value("optionIds").toArray().isEmpty()) {
                const QString reason = QStringLiteral("答案表中同一题号存在冲突答案，未自动选择；请确认本题答案");
                reviewReason = reviewReason.isEmpty() ? reason : reviewReason + QStringLiteral("；") + reason;
                auto review = question.value("review").toObject();
                review.insert("needsReview", true);
                review.insert("riskLevel", "hard");
                review.insert("reason", reviewReason);
                question.insert("review", review);
            }
            const QJsonObject stemImage = question.value("stemImage").toObject();
            const QString assetPath = stemImage.value("path").toString();
            if (!assetPath.isEmpty() && !result.assets.contains(assetPath)) {
                const int page = question.value("source").toObject().value("page").toInt();
                if (document.pageImages.contains(page))
                    result.assets.insert(assetPath, document.pageImages.value(page));
            }
            if (reviewReason.isEmpty())
                result.questions.append(question);
            else
                result.needsReviewQuestions.append(question);
            ++processedQuestions;
            reportProgress(index + 1);
        }
    }

    // 删除没有任何题目引用的材料，保证正常候选进入统一校验器时不会因孤立材料失败。
    QSet<QString> referenced;
    for (const QJsonArray questions : {result.questions, result.needsReviewQuestions})
        for (const auto& value : questions) {
            const QString id = value.toObject().value("materialId").toString();
            if (!id.isEmpty())
                referenced.insert(id);
        }
    QJsonArray usedMaterials;
    for (const auto& value : result.materials)
        if (referenced.contains(value.toObject().value("id").toString()))
            usedMaterials.append(value);
    result.materials = usedMaterials;
    applyRuleBasedGenerationAudit(&result, hasAnswerKey);
    return result;
}

} // namespace quizpane::studio
