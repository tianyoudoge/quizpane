#include "quizpane/studio/rule_based_generator.hpp"
#include "quizpane/studio/option_label.hpp"

#include <QFileInfo>
#include <QBuffer>
#include <QCryptographicHash>
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QPainter>
#include <QRegularExpression>
#include <QSet>

#include <limits>

namespace quizpane::studio {
namespace {

struct SourceLine {
    QString text;
    int page = 0;
};

struct QuestionAnchor {
    int line = 0;
    int number = 0;
    QString firstStemLine;
};

struct MaterialMarker {
    int line = 0;
    int contentLine = 0;
    int firstQuestionLine = -1;
    int firstNumber = 0;
    int lastNumber = 0;
    QString id;
};

const QRegularExpression& questionPattern() {
    static const QRegularExpression value(QStringLiteral(
        R"(^\s*(?:(?:问题|题目)\s*)?(?:第\s*)?(\d{1,4})\s*(?:题|[．、:：\)）]|\.(?!\d))\s*(.*)$)"));
    return value;
}

const QRegularExpression& inlineAnswerPattern() {
    static const QRegularExpression value(QStringLiteral(
        R"(^\s*(?:【?\s*(?:(?:参考|标准)?答案)\s*】?|正确答案)\s*[:：]?\s*(.+?)\s*$)"));
    return value;
}

const QRegularExpression& solutionPattern() {
    static const QRegularExpression value(
        QStringLiteral(R"(^\s*(?:【?\s*(?:答案)?解析\s*】?|解答|说明)\s*[:：]?\s*(.*)$)"));
    return value;
}

bool isAnswerSectionHeader(const QString& text) {
    static const QRegularExpression pattern(
        QStringLiteral(R"(^\s*(?:答案|参考答案|答案汇总|答案及解析|参考答案及解析)\s*[:：]?\s*$)"));
    return pattern.match(text).hasMatch();
}

bool isMaterialHeader(const QString& text) {
    static const QRegularExpression pattern(QStringLiteral(
        R"(^\s*(?:[（(][一二三四五六七八九十\d]+[）)]\s*)?(?:(?:材料|资料|阅读材料)\s*[一二三四五六七八九十\d]*\s*[:：]?|阅读(?:下列|以下)(?:材料|文字)|根据(?:下列|以下)(?:统计)?(?:资料|材料)|原文\s*[:：])\s*.*$)"));
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

QRectF questionBoundsFor(const ExtractedDocument& document, int page, int number) {
    const auto& anchors = document.questionAnchors.value(page);
    for (const PdfTextAnchor& anchor : anchors)
        if (anchor.text.toInt() == number)
            return anchor.bounds;
    return {};
}

QRectF lineBoundsFor(const ExtractedDocument& document, int page, const QString& text) {
    const QString wanted = text.trimmed();
    for (const PdfTextAnchor& anchor : document.lineAnchors.value(page))
        if (anchor.text == wanted)
            return anchor.bounds;
    return {};
}

QString restoreDroppedBlankLines(QString value) {
    // 【甲】/【乙】/【丙】是原题用来指代三个填词位置的编号，必须原样保留；
    // 它们不是待输入框。只有 PDF 文字层丢掉的空白横线才补成可渲染的占位。
    // 很多 PDF 把空白横线作为单独的矢量/图片对象，文字层只留下“的 。”。
    // 仅在汉字与紧随的中文标点之间补占位，避免把正常的段落空格误判成填空。
    static const QRegularExpression droppedUnderline(
        QStringLiteral(R"((\p{Han})[ \t]+([，。；、]))"));
    value.replace(droppedUnderline, QStringLiteral("\\1〔填空〕\\2"));
    return value;
}

QJsonArray extractMaterialLayoutImages(const ExtractedDocument& document, int firstPage,
                                       int lastPage, const QString& firstLine,
                                       const QString& firstQuestionLine,
                                       const QString& materialId,
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
        const QRect crop(qFloor(source.width() * 0.04), qFloor(source.height() * top),
                         qCeil(source.width() * 0.92),
                         qCeil(source.height() * (bottom - top)));
        const QImage snippet = source.copy(crop.intersected(source.rect()));
        QByteArray png;
        QBuffer buffer(&png);
        if (snippet.isNull() || !buffer.open(QIODevice::WriteOnly) || !snippet.save(&buffer, "PNG"))
            continue;
        const QString path = QStringLiteral("assets/%1-%2-p%3.png")
            .arg(assetBaseName(document.sourcePath), materialId).arg(page);
        assets->insert(path, png);
        images.append(QJsonObject{{"path", path}, {"alt", "原卷材料版式（含下划线和填空）"}});
    }
    return images;
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
        const QRect crop(qBound(0, qFloor(left * source.width()), source.width() - 1),
                         qBound(0, qFloor((rowY - 0.065) * source.height()), source.height() - 1),
                         qMax(1, qCeil((right - left) * source.width())),
                         qMax(1, qCeil(0.09 * source.height())));
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
        result.insert(ids.at(index), QJsonObject{{"path", path}, {"alt", QStringLiteral("选项%1")
            .arg(ids.at(index).toUpper())}});
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
                                       const QString& questionLine,
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
    const QRectF fullQuestionLineBounds = lineBoundsFor(document, page, questionLine);
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
    if (optionRow.size() == 4) {
        const qreal rowY = optionRow.value(QStringLiteral("a")).center().y();
        // 文字标签基线正好位于分数下方；约 2.8% 页高足以覆盖分子/分母，
        // 下方只留 1% 页高，避免把紧随其后的第 77 题题干卷入。
        top = qMax<qreal>(0.0, rowY - 0.028);
        const qreal optionBottom = qMin<qreal>(0.995, rowY + 0.008);
        bottom = !nextBounds.isEmpty()
            ? qMin(optionBottom, nextBounds.top() - 0.001) : optionBottom;
        const qreal firstCenter = optionRow.value(QStringLiteral("a")).center().x();
        const qreal lastCenter = optionRow.value(QStringLiteral("d")).center().x();
        const qreal spacing = (lastCenter - firstCenter) / 3.0;
        cropLeft = qMax<qreal>(0.0, optionRow.value(QStringLiteral("a")).left());
        cropRight = qMin<qreal>(1.0,
            optionRow.value(QStringLiteral("d")).right() + spacing * 0.55);
    }
    if (bottom <= top + 0.035)
        return {};
    const QRect crop(qFloor(source.width() * cropLeft), qFloor(source.height() * top),
                     qCeil(source.width() * (cropRight - cropLeft)),
                     qCeil(source.height() * (bottom - top)));
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
    return {{"path", path}, {"alt", "原卷题图（含图形选项）"}};
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
    int page = document.hasPageBoundaries ? 1 : 0;
    QString current;
    const QString normalized = document.plainText.normalized(QString::NormalizationForm_C);
    for (const QChar ch : normalized) {
        if (ch == u'\f') {
            result.append({current.trimmed(), page});
            current.clear();
            if (document.hasPageBoundaries)
                ++page;
        } else if (ch == u'\n' || ch == u'\r') {
            if (!current.isEmpty()) {
                QString cleaned = current.trimmed();
                // PDF 页脚通常是 "- 15 -" / "-15-"，绝不能进入题干；保留
                // 其它换行以免把资料题的段落硬拼成一行。
                static const QRegularExpression pageFooter(QStringLiteral(R"(^[-—–\s]*\d{1,4}[-—–\s]*$)"));
                if (!pageFooter.match(cleaned).hasMatch()) result.append({cleaned, page});
            }
            current.clear();
        } else {
            current += ch;
        }
    }
    if (!current.isEmpty()) result.append({current.trimmed(), page});
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

QHash<int, QString> globalAnswers(const QList<SourceLine>& lines, int sectionStart, int sectionEnd) {
    QHash<int, QString> answers;
    if (sectionStart < 0)
        return answers;
    const int limit = sectionEnd >= 0 ? sectionEnd : lines.size();
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
    for (int index = sectionStart + 1; index < limit && index < lines.size(); ++index) {
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
            if (!answer.isEmpty() && !answers.contains(number))
                answers.insert(number, answer);
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
    for (int index = sectionStart + 1; index < lineLimit && index < lines.size(); ++index) {
        const QString line = lines.at(index).text;
        const auto record = answerRecord.match(line);
        if (record.hasMatch()) currentNumber = record.captured(1).toInt();
        const auto rangeMatch = range.match(line);
        if (rangeMatch.hasMatch()) {
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
                    if (!answers.contains(number))
                        answers.insert(number, QString(values.at(number - first)));
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
            if (number > 0 && !answer.isEmpty() && !answers.contains(number))
                answers.insert(number, answer);
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
            if (number > 0 && !label.isEmpty() && !answers.contains(number))
                answers.insert(number, label);
        }
        // 很多真题解析不是“1. A”式答案汇总，而是在题目解析末尾写“故正确
        // 答案为 C”。用最近一个题号归属这条结论，兼容题目文件与答案文件分离。
        const auto narrative = narrativeAnswer.match(line);
        if (currentNumber > 0 && narrative.hasMatch() && !answers.contains(currentNumber)) {
            const QString answer = normalizeAnswer(narrative.captured(1));
            if (!answer.isEmpty()) answers.insert(currentNumber, answer);
        }
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
        R"(|([①②③④⑤⑥⑦⑧⑨⑩])\s*[:：\.．、\)）\]」』]?\s*)"          // 圈码 + 可选分隔
        R"(|([①②③④⑤⑥⑦⑧⑨⑩])\s+)"                                  // 圈码 + 空格分隔
        R"(|[（(\[{「『]\s*([1-9])\s*[)）\]}」』]\s*)"                  // (1) 形式（各种括号）
        R"(|(?<![A-Za-z0-9])([1-9])\s*[)）\]」』]\s*)"));               // 1) 形式
    QList<QRegularExpressionMatch> markers;
    auto iterator = marker.globalMatch(line);
    while (iterator.hasNext()) {
        const auto match = iterator.next();
        if (!allowNumericLabels && match.captured(1).isEmpty() &&
            match.captured(2).isEmpty() && match.captured(3).isEmpty())
            continue;
        markers.append(match);
    }
    if (markers.isEmpty())
        return {};
    const QString lead = line.left(markers.first().capturedStart());
    const bool hasLeadText = !lead.trimmed().isEmpty();
    // 单 marker 且前面有文字 → 视为题干，不当选项切。
    if (markers.size() == 1 && hasLeadText)
        return {};
    // ≥2 marker → 切分；若有 lead 文本则作为题干前缀回传。
    if (prefix && hasLeadText)
        *prefix = lead.trimmed();
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

// 题干/选项行末尾带的答案抽取。返回归一化后的答案字母串，空表示未识别。
// 支持两种尾随形式：
//   1) “答案：A” / “【答案】AB” 这类带答案词的；
//   2) “（A）” / “(A)” / “（AB）” 这类题干末尾直接括号写答案的（选择题写法）。
// 优先级低于以答案词开头的整行答案。
QString trailingAnswer(const QString& line) {
    static const QRegularExpression wordAnswer(QStringLiteral(
        R"((?:【?\s*(?:参考|标准)?答案\s*】?|正确答案)\s*[:：]\s*([A-Fa-f]{1,6})\s*$)"));
    auto match = wordAnswer.match(line);
    if (match.hasMatch())
        return normalizeAnswer(match.captured(1));
    // 题干末尾括号答案：（A） 「AB」 [A] {A}，括号兼容各种书写习惯。要求括号前
    // 有题干文字（start!=0），避免把整行选项“(A) ...”误判为答案。
    static const QRegularExpression bracketAnswer(QStringLiteral(
        R"([（(\[{「『]\s*([A-Fa-f]{1,6})\s*[)）\]}」』]\s*$)"));
    match = bracketAnswer.match(line);
    if (match.hasMatch()) {
        const int start = match.capturedStart();
        if (start == 0)
            return {};
        return normalizeAnswer(match.captured(1));
    }
    return {};
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

QJsonObject parseQuestion(const ExtractedDocument& document, const QList<SourceLine>& lines,
                          const QuestionAnchor& anchor, int blockEnd, const QString& stableId,
                          const QString& materialId, const QHash<int, QString>& answerKey,
                          const QHash<int, QString>& solutionKey,
                          QHash<QString, QByteArray>* generatedAssets, QString* reviewReason,
                          bool allowMultipleAnswers) {
    QStringList stemLines;
    QList<QPair<QString, QString>> options;
    QString rawAnswer;
    QStringList solutionLines;
    bool inSolution = false;
    bool afterOptions = false;
    static const QRegularExpression explicitLetterOption(QStringLiteral(
        R"((?:^|\s)[A-Fa-f]\s*[\.．、:：\)）])"));
    bool hasExplicitLetterOptions =
        explicitLetterOption.match(anchor.firstStemLine).hasMatch();
    for (int index = anchor.line + 1; index < blockEnd && !hasExplicitLetterOptions; ++index)
        hasExplicitLetterOptions = explicitLetterOption.match(lines.at(index).text).hasMatch();
    const bool allowNumericOptionLabels = !hasExplicitLetterOptions;
    QString firstPrefix;
    if (!anchor.firstStemLine.isEmpty()) {
        // 题号行可能同行带选项：1. 题干 A.甲 B.乙…。先抽选项，剩余文本作为题干。
        const auto firstOptions =
            optionsOnLine(anchor.firstStemLine, &firstPrefix, allowNumericOptionLabels);
        if (!firstOptions.isEmpty())
            options += firstOptions;
        const QString tailAnswer = trailingAnswer(anchor.firstStemLine);
        if (!tailAnswer.isEmpty())
            rawAnswer = tailAnswer;
        // 题干前缀：优先用切选项后剩的前缀，否则用整行（移除尾随答案）。
        if (!firstPrefix.isEmpty())
            stemLines.append(firstPrefix);
        else {
            const auto answerMatch = inlineAnswerPattern().match(anchor.firstStemLine);
            if (answerMatch.hasMatch()) {
                rawAnswer = answerMatch.captured(1).trimmed();
            } else {
                QString stem = anchor.firstStemLine;
                static const QRegularExpression stripTrailing(QStringLiteral(
                    R"((?:【?\s*(?:参考|标准)?答案\s*】?|正确答案)\s*[:：]\s*[A-Fa-f]{1,6}\s*$)"));
                stem.remove(stripTrailing);
                if (!stem.trimmed().isEmpty())
                    stemLines.append(stem.trimmed());
            }
        }
    }

    for (int index = anchor.line + 1; index < blockEnd; ++index) {
        const QString line = lines.at(index).text.trimmed();
        if (line.isEmpty())
            continue;
        const auto answerMatch = inlineAnswerPattern().match(line);
        if (answerMatch.hasMatch()) {
            rawAnswer = answerMatch.captured(1).trimmed();
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
        // 行尾可能带尾随答案（“答案：A”或题干末尾“（A）”括号答案）。先剥掉再
        // 切选项，避免把答案文本粘到最后一个选项文本末尾。
        static const QRegularExpression trailingAnswerCut(QStringLiteral(
            R"(\s*(?:【?\s*(?:参考|标准)?答案\s*】?|正确答案)\s*[:：]\s*[A-Fa-f]{1,6}\s*$"
            R"(|[（(]\s*[A-Fa-f]{1,6}\s*[)）]\s*$))"));
        QString lineForOptions = line;
        const auto cutMatch = trailingAnswerCut.match(lineForOptions);
        if (cutMatch.hasMatch()) {
            lineForOptions = lineForOptions.left(cutMatch.capturedStart()).trimmed();
            const QString tailAnswer = trailingAnswer(line);
            if (rawAnswer.isEmpty() && !tailAnswer.isEmpty())
                rawAnswer = tailAnswer;
        }
        const auto parsedOptions =
            optionsOnLine(lineForOptions, nullptr, allowNumericOptionLabels);
        if (!parsedOptions.isEmpty()) {
            options += parsedOptions;
            afterOptions = true;
        } else if (afterOptions && !options.isEmpty()) {
            // 选项已出现后的续行：仍可能含尾随答案，已剥过；归入最后选项文本。
            options.last().second += QStringLiteral("\n") + lineForOptions;
        } else {
            stemLines.append(lineForOptions.isEmpty() ? line : lineForOptions);
        }
    }
    QString answer = answerFromOptionText(rawAnswer, options);
    if (answer.isEmpty())
        answer = answerKey.value(anchor.number);
    if (solutionLines.isEmpty() && solutionKey.contains(anchor.number))
        solutionLines.append(solutionKey.value(anchor.number));

    const QString visualText = stemLines.join(QChar('\n'));
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
        document.pageImages.contains(sourcePage) &&
        (visualText.contains(QStringLiteral("图")) || visualText.contains(QStringLiteral("表")) ||
         visualText.contains(QStringLiteral("统计")) || visualText.contains(QStringLiteral("问号")));
    const bool needsVisualOptions = options.size() < 2 &&
        (hasVisualOptionLabels || hasVisualContext);
    QHash<QString, QJsonObject> optionImages;
    bool attachStemImage = hasVisualContext;
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
    }

    QJsonArray jsonOptions;
    QSet<QString> optionIds;
    for (const auto& option : options) {
        if (optionIds.contains(option.first))
            continue;
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
    if (stemLines.join(QChar('\n')).trimmed().isEmpty())
        reasons.append(QStringLiteral("缺少题干"));
    if (jsonOptions.size() < 2)
        reasons.append(QStringLiteral("未识别到至少两个完整选项"));
    if (answer.isEmpty())
        reasons.append(rawAnswer.isEmpty() ? QStringLiteral("未识别到答案")
                                           : QStringLiteral("答案文本无法唯一匹配选项"));
    else if (answerIds.size() != answer.size())
        reasons.append(QStringLiteral("答案引用了不存在的选项"));
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
    if (answerIds.size() > 1 && !allowsMultiple)
        reasons.append(QStringLiteral("题干未标注多选题，却识别到多个答案"));
    if (strictMultipleInStem && !indefiniteInStem && answerIds.size() < 2)
        reasons.append(QStringLiteral("多选题答案少于两个选项"));
    QString type = QStringLiteral("single_choice");
    // 判断题优先判定：空括号“（ ）”合成 正确/不正确 两个选项的题，直接定型，
    // 不依赖选项文本里是否同时出现“正确/错误”（“不正确”不含“错误”，旧检测会漏）。
    if (forcedTrueFalse)
        type = QStringLiteral("true_false");
    else if (allowsMultiple && answerIds.size() > 1)
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
    QJsonObject question{{"id", stableId},
                         {"catalogId", "generated"},
                         {"type", type},
                         {"stem", stemLines.join(QChar('\n')).trimmed()},
                         {"options", jsonOptions},
                         {"answer", QJsonObject{{"optionIds", answerIds}}},
                         {"solution", solutionLines.join(QChar('\n')).trimmed()},
                         {"source", source}};
    if (!materialId.isEmpty())
        question.insert("materialId", materialId);
    // 图形推理、统计资料和明确提到图/表的题保留原卷可视内容。无可靠选项文字
    // 锚点时，使用“本题到下一题前”的裁切图，而不是把整页试卷挂到题干下。
    if (attachStemImage) {
        const QJsonObject visualImage = extractQuestionVisualImage(
            document, sourcePage, anchor.number, lines.at(anchor.line).text, generatedAssets);
        if (!visualImage.isEmpty())
            question.insert("stemImage", visualImage);
        else {
            // 只有测试夹具或损坏 PDF 缺少题号坐标时才保留历史整页回退；真实
            // PDF 一旦有题号锚点必走上面的局部裁切，不能把相邻题目带进来。
            const QString path = QStringLiteral("assets/%1-p%2.png")
                .arg(assetBaseName(document.sourcePath)).arg(sourcePage);
            question.insert("stemImage", QJsonObject{{"path", path}, {"alt", "原卷图表"}});
        }
    }
    if (!reasons.isEmpty()) {
        *reviewReason = reasons.join(QStringLiteral("；"));
        question.insert(
            "review",
            QJsonObject{{"needsReview", true}, {"confidence", 0.25}, {"reason", *reviewReason}});
    } else {
        question.insert("review", QJsonObject{{"needsReview", false},
                                              {"confidence", document.usedOcr ? 0.75 : 0.95}});
    }
    return question;
}

} // namespace

RuleBasedGenerationResult
RuleBasedBankGenerator::generate(const QList<ExtractedDocument>& documents) const {
    RuleBasedGenerationResult result;
    int documentOrdinal = 0;
    for (const auto& document : documents) {
        ++documentOrdinal;
        const QList<SourceLine> lines = sourceLines(document);

        // 收集所有答案区头行号。整体前后分开的文件只有一个，阶段分组的文件
        // 会有多个（每个阶段一组题+一组答案）。答案区头本身不作为题目终点，
        // 但它界定了“从这里开始进入答案文本”。
        QList<int> answerSections;
        for (int index = 0; index < lines.size(); ++index) {
            if (isAnswerSectionHeader(lines.at(index).text))
                answerSections.append(index);
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
        static const QRegularExpression answerRecordLine(QStringLiteral(
            R"(^\s*(?:第\s*)?(\d{1,4})\s*(?:题|[\.．、:：\)）])\s*(?:【?答案】?\s*[:：]?)?\s*[A-Fa-f]{1,6}\s*$)"));
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
            anchors.append({index, number, match.captured(2).trimmed()});
        }

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
            QStringList body;
            for (int cursor = contentLine + 1; cursor < firstQuestionLine; ++cursor)
                if (!lines.at(cursor).text.trimmed().isEmpty())
                    body.append(lines.at(cursor).text);
            if (body.isEmpty())
                body.append(lines.at(contentLine).text);
            QJsonObject source{{"document", QFileInfo(document.sourcePath).fileName()}};
            if (lines.at(line).page > 0)
                source.insert("page", lines.at(line).page);
            const QString materialBody = restoreDroppedBlankLines(body.join(QChar('\n')).trimmed());
            QJsonObject material{{"id", id}, {"catalogId", "generated"},
                                 {"title", lines.at(contentLine).text.left(200)},
                                 {"body", materialBody}, {"source", source}};
            const int firstPage = lines.at(line).page;
            const int lastPage = lines.at(firstQuestionLine).page;
            // 对 PDF 阅读材料保存裁切后的原卷版式。文字层无法表示下划线样式、
            // 空白横线和嵌入式图片横线；这一层视觉附件确保它们不再丢失，同时
            // 只裁材料范围，绝不把整页试卷错挂到子题题干。
            QJsonArray images = extractMaterialLayoutImages(
                document, firstPage, lastPage, lines.at(line).text,
                lines.at(firstQuestionLine).text, id, &result.assets);
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
                    images.append(QJsonObject{{"path", path}, {"alt", "原卷资料图表"}});
                    result.assets.insert(path, document.pageImages.value(page));
                }
            }
            if (!images.isEmpty()) material.insert("images", images);
            result.materials.append(material);
        }

        // 逐段解析答案区。每段的作用域到下一个题号锚点或下一个材料头为止，
        // 这样阶段二的题目不会被阶段一的答案区吞掉。
        QHash<int, QString> answers;
        QHash<int, QString> solutions;
        for (int sectionIndex = 0; sectionIndex < answerSections.size(); ++sectionIndex) {
            const int sectionStart = answerSections.at(sectionIndex);
            const int sectionEnd =
                answerSectionEnd(lines, sectionStart, anchors, materialMarkers);
            const auto segmentAnswers = globalAnswers(lines, sectionStart, sectionEnd);
            for (auto it = segmentAnswers.cbegin(); it != segmentAnswers.cend(); ++it)
                if (!answers.contains(it.key()))
                    answers.insert(it.key(), it.value());
            const auto segmentSolutions = globalSolutions(lines, sectionStart, sectionEnd);
            for (auto it = segmentSolutions.cbegin(); it != segmentSolutions.cend(); ++it)
                if (!solutions.contains(it.key()))
                    solutions.insert(it.key(), it.value());
        }

        if (anchors.isEmpty()) {
            result.warnings.append(QStringLiteral("%1：没有识别到题号锚点")
                                       .arg(QFileInfo(document.sourcePath).fileName()));
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
            QString reviewReason;
            QJsonObject question = parseQuestion(document, lines, anchor, blockEnd, id, materialId,
                                                 answers, solutions, &result.assets, &reviewReason,
                                                 allowMultipleAnswers);
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
    return result;
}

} // namespace quizpane::studio
