#include "../apps/desktop-qt/src/ui/ai_response_format.hpp"
#include "../apps/desktop-qt/src/ui/math_formula.hpp"

#include <QApplication>
#include <QRegularExpression>

namespace {

using quizpane::ui::MathStyle;
using quizpane::ui::renderMath;

int failure(int code, int line) {
    qCritical("Formula assertion %d failed at line %d", code, line);
    return code;
}

MathStyle style() {
    MathStyle s;
    s.font.setPixelSize(14);
    s.color = Qt::black;
    return s;
}

int imageCount(const QString& html) {
    return static_cast<int>(html.count(QStringLiteral("<img src=\"data:image/png;base64,")));
}

// 统计每一行的墨迹像素，用来判断分数是否上下叠放。
QList<int> inkRows(const QImage& image) {
    QList<int> rows;
    for (int y = 0; y < image.height(); ++y) {
        int ink = 0;
        for (int x = 0; x < image.width(); ++x) ink += qAlpha(image.pixel(x, y)) > 128;
        rows.append(ink);
    }
    return rows;
}

int inkHeight(const QImage& image) {
    const QList<int> rows = inkRows(image);
    int top = -1, bottom = -1;
    for (int y = 0; y < rows.size(); ++y) {
        if (rows.at(y) == 0) continue;
        if (top < 0) top = y;
        bottom = y;
    }
    return top < 0 ? 0 : bottom - top + 1;
}

// 分数线：一段宽度接近整幅图片、上下两侧都有字形的墨迹行。
bool hasStackedFraction(const QImage& image) {
    const QList<int> rows = inkRows(image);
    int lineRow = -1;
    for (int y = 0; y < rows.size(); ++y)
        if (rows.at(y) > image.width() * 0.6) lineRow = y;
    if (lineRow < 0) return false;
    int above = 0, below = 0;
    for (int y = 0; y < lineRow - 1; ++y) above += rows.at(y);
    for (int y = lineRow + 2; y < rows.size(); ++y) below += rows.at(y);
    return above > 10 && below > 10;
}

}  // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    const MathStyle s = style();

    // 简单变量、不等式不生成图片，避免撑高行距。
    if (imageCount(quizpane::ui::mathHtml(QStringLiteral("a"), s, false)) != 0) return failure(1, __LINE__);
    if (imageCount(quizpane::ui::mathHtml(QStringLiteral("a>b"), s, false)) != 0) return failure(2, __LINE__);
    if (!quizpane::ui::mathHtml(QStringLiteral("x^{2}"), s, false).contains(QStringLiteral("<sup>"))) return failure(3, __LINE__);

    // 同比增长率、比重差、嵌套分数都画成上下叠放的分数。
    for (const QString& latex : {QStringLiteral("\\frac{本期-上期}{上期}\\times100\\%"),
                                 QStringLiteral("\\frac{A}{B}\\times\\frac{a-b}{1+a}"),
                                 QStringLiteral("\\frac{6240-5800}{5800}\\approx7.59\\%")}) {
        if (imageCount(quizpane::ui::mathHtml(latex, s, false)) != 1) return failure(4, __LINE__);
        if (!hasStackedFraction(renderMath(latex, s, false).image)) return failure(5, __LINE__);
    }
    const auto nested = renderMath(QStringLiteral("\\frac{\\frac{440}{5800}}{\\frac{20}{460}}"), s, true);
    const auto single = renderMath(QStringLiteral("\\frac{440}{5800}"), s, true);
    if (nested.height <= single.height * 1.5) return failure(6, __LINE__);

    // 括号包住分数时随内容拉伸。
    const auto tallParens = renderMath(QStringLiteral("\\left(1+\\frac{r}{100}\\right)"), s, false);
    const auto plainParens = renderMath(QStringLiteral("(1+r)"), s, false);
    const auto fracOnly = renderMath(QStringLiteral("\\frac{r}{100}"), s, false);
    if (inkHeight(tallParens.image) < inkHeight(fracOnly.image) ||
        inkHeight(tallParens.image) <= inkHeight(plainParens.image) * 1.3) return failure(7, __LINE__);

    // 超宽公式按可用宽度缩小。
    MathStyle narrow = s;
    const QString wide = QStringLiteral("\\frac{480.2-57.4}{480.2}\\times100\\%+\\frac{6240-5800}{5800}\\times100\\%");
    const int fullWidth = renderMath(wide, s, true).width;
    narrow.maxWidth = fullWidth * 3 / 4;
    if (renderMath(wide, narrow, true).width > narrow.maxWidth) return failure(8, __LINE__);

    // 高分屏按设备像素比出图。
    MathStyle retina = s;
    retina.devicePixelRatio = 2;
    const auto hiDpi = renderMath(QStringLiteral("\\frac{a}{b}"), retina, false);
    if (hiDpi.image.width() != hiDpi.width * 2) return failure(9, __LINE__);

    // 未支持的命令保留源码，而不是悄悄吞掉。
    const QString unknown = quizpane::ui::mathHtml(QStringLiteral("\\foo{x}"), s, false);
    if (!unknown.contains(QStringLiteral("\\foo"))) return failure(10, __LINE__);

    // 完整回答：分节标题、列表、粗体、行内与独立公式，以及漏写 $ 的裸 \frac。
    quizpane::ui::AiResponseStyle responseStyle;
    responseStyle.math = s;
    responseStyle.accent = QColor(0x2d, 0x6b, 0x4f);
    responseStyle.tipAccent = QColor(0x9a, 0x6b, 0x1f);
    const QString html = quizpane::ui::formatAiResponse(QStringLiteral(
        "【答案确认】正确答案为 **A**。\n\n"
        "【解题步骤】\n"
        "- 部分增速为 $a$，整体增速为 $b$。\n"
        "- 如果 $a>b$，比重**上升**；变化量小于 $|a-b|$。\n"
        "- *进阶公式*：两期比重差 = $\\frac{A}{B}\\times\\frac{a-b}{1+a}$。\n\n"
        "$$\\frac{6240-5800}{5800}\\times100\\%\\approx7.59\\%$$\n\n"
        "【速算技巧】增长率 = \\frac{440}{5800}，约为 7.6%。"), responseStyle);
    if (html.count(QStringLiteral("•&nbsp;")) != 3) return failure(11, __LINE__);
    if (!html.contains(QStringLiteral("<b>A</b>")) || !html.contains(QStringLiteral("<b>上升</b>"))) return failure(12, __LINE__);
    if (!html.contains(QStringLiteral("<i>进阶公式</i>"))) return failure(13, __LINE__);
    if (!html.contains(QStringLiteral("【答案确认】")) || !html.contains(QStringLiteral("#9a6b1f"))) return failure(14, __LINE__);
    if (imageCount(html) != 3) return failure(15, __LINE__);
    if (html.contains(QLatin1Char('$')) || html.contains(QStringLiteral("\\frac"))) return failure(16, __LINE__);
    if (html.contains(QChar(0xE000))) return failure(17, __LINE__);

    // 标签保存原文，字体变化后重新出图。
    quizpane::ui::AiResponseLabel label;
    label.resize(300, 100);
    label.setResponse(QStringLiteral("比重差 $\\frac{A}{B}$"));
    const QString before = label.text();
    QFont bigger = label.font();
    bigger.setPixelSize(22);
    label.setFont(bigger);
    if (label.text() == before || imageCount(label.text()) != 1) return failure(18, __LINE__);
    label.setMessage(QStringLiteral("请求失败"));
    if (!label.response().isEmpty()) return failure(19, __LINE__);
    return 0;
}
