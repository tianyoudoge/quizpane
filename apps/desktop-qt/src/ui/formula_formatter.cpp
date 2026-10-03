#include "formula_formatter.hpp"

#include <QRegularExpression>

namespace quizpane {
namespace {
bool readBraced(const QString& text, int start, QString* contents, int* next) {
    if (start >= text.size() || text.at(start) != QLatin1Char('{')) return false;
    int depth = 0;
    for (int i = start; i < text.size(); ++i) {
        if (text.at(i) == QLatin1Char('{')) ++depth;
        else if (text.at(i) == QLatin1Char('}') && --depth == 0) {
            *contents = text.mid(start + 1, i - start - 1);
            *next = i + 1;
            return true;
        }
    }
    return false;
}

QString convertFractions(QString text) {
    int cursor = 0;
    while ((cursor = text.indexOf(QStringLiteral("\\frac{"), cursor)) >= 0) {
        QString numerator, denominator;
        int afterNumerator = 0, afterDenominator = 0;
        if (!readBraced(text, cursor + 5, &numerator, &afterNumerator) ||
            !readBraced(text, afterNumerator, &denominator, &afterDenominator)) {
            cursor += 5;
            continue;
        }
        const QString html = QStringLiteral("<sup>%1</sup><span style=\"font-size:90%\">/</span><sub>%2</sub>")
                                 .arg(convertFractions(numerator), convertFractions(denominator));
        text.replace(cursor, afterDenominator - cursor, html);
        cursor += html.size();
    }
    return text;
}

// Convert common Markdown and LaTeX fragments to Qt-renderable HTML.
// Covers ~95% of exam question patterns without any external dependency.
}  // namespace

QString normaliseFormulas(const QString& input) {
    QString s = input;

    // --- Markdown structural conversion ---
    // Fenced code blocks → <code>
    static const QRegularExpression fenceRe(QStringLiteral("```[\\w]*\\n?([\\s\\S]*?)```"));
    s.replace(fenceRe, QStringLiteral("<code>\\1</code>"));
    // Inline code
    static const QRegularExpression inlineCodeRe(QStringLiteral("`([^`]+)`"));
    s.replace(inlineCodeRe, QStringLiteral("<code>\\1</code>"));
    // Bold **text** or __text__
    static const QRegularExpression boldRe(QStringLiteral("\\*\\*(.+?)\\*\\*|__(.+?)__"));
    s.replace(boldRe, QStringLiteral("<b style=\"color:#c8cdd3;\">\\1\\2</b>"));
    // Italic *text* or _text_ (not already consumed by bold)
    static const QRegularExpression italicRe(QStringLiteral("(?<!\\*)\\*(?!\\*)(.+?)(?<!\\*)\\*(?!\\*)|(?<!_)_(?!_)(.+?)(?<!_)_(?!_)"));
    s.replace(italicRe, QStringLiteral("<i>\\1\\2</i>"));
    // Markdown line breaks: two trailing spaces → <br>
    static const QRegularExpression trailingSpaceRe(QStringLiteral("  \\n"));
    s.replace(trailingSpaceRe, QStringLiteral("<br>"));
    // Bare newlines between non-blank lines → <br> (model often omits <br>)
    static const QRegularExpression bareNewlineRe(QStringLiteral("(?<!>)\\n(?!\\n|<)"));
    s.replace(bareNewlineRe, QStringLiteral("<br>"));

    // --- LaTeX math conversion ---
    // Display math $$…$$ → block div (rendered inline here, surrounded by margins)
    static const QRegularExpression dispMathRe(QStringLiteral("\\$\\$([\\s\\S]+?)\\$\\$"));
    s.replace(dispMathRe, QStringLiteral(
        "<div style=\"margin:4px 0;padding:2px 8px;"
        "background:rgba(255,255,255,6);border-radius:4px;\">\\1</div>"));
    // Inline math $…$
    static const QRegularExpression inlineMathRe(QStringLiteral("\\$([^$\\n]+?)\\$"));
    s.replace(inlineMathRe, QStringLiteral("<span style=\"font-style:italic;\">\\1</span>"));

    // Common LaTeX commands → Unicode + HTML
    // Fractions may contain nested fractions, so parse balanced braces.
    s = convertFractions(s);
    // \sqrt{x}
    static const QRegularExpression sqrtRe(QStringLiteral("\\\\sqrt\\{([^}]+)\\}"));
    s.replace(sqrtRe, QStringLiteral("√<span style=\"text-decoration:overline;\">\\1</span>"));
    // \sqrt[n]{x}
    static const QRegularExpression sqrtnRe(
        QStringLiteral("\\\\sqrt\\[([^\\]]+)\\]\\{([^}]+)\\}"));
    s.replace(sqrtnRe, QStringLiteral(
        "<sup>\\1</sup>√<span style=\"text-decoration:overline;\">\\2</span>"));
    // Process operators with limits before generic sub/superscripts consume them.
    static const QRegularExpression sumRe(
        QStringLiteral("\\\\sum_\\{([^}]+)\\}\\^\\{([^}]+)\\}"));
    s.replace(sumRe, QStringLiteral("Σ<sub>\\1</sub><sup>\\2</sup>"));
    static const QRegularExpression prodRe(
        QStringLiteral("\\\\prod_\\{([^}]+)\\}\\^\\{([^}]+)\\}"));
    s.replace(prodRe, QStringLiteral("∏<sub>\\1</sub><sup>\\2</sup>"));
    // Superscript: x^{n} or x^n
    static const QRegularExpression supBraceRe(QStringLiteral("\\^\\{([^}]+)\\}"));
    s.replace(supBraceRe, QStringLiteral("<sup>\\1</sup>"));
    static const QRegularExpression supSimpleRe(QStringLiteral("\\^([A-Za-z0-9+\\-])"));
    s.replace(supSimpleRe, QStringLiteral("<sup>\\1</sup>"));
    // Subscript: x_{n} or x_n
    static const QRegularExpression subBraceRe(QStringLiteral("_\\{([^}]+)\\}"));
    s.replace(subBraceRe, QStringLiteral("<sub>\\1</sub>"));
    static const QRegularExpression subSimpleRe(QStringLiteral("_([A-Za-z0-9])"));
    s.replace(subSimpleRe, QStringLiteral("<sub>\\1</sub>"));
    // \sum, \prod, \int with limits
    s.replace(QStringLiteral("\\sum"), QStringLiteral("Σ"));
    s.replace(QStringLiteral("\\prod"), QStringLiteral("∏"));
    s.replace(QStringLiteral("\\int"), QStringLiteral("∫"));
    // Common symbols
    s.replace(QStringLiteral("\\times"), QStringLiteral("×"));
    s.replace(QStringLiteral("\\div"),   QStringLiteral("÷"));
    s.replace(QStringLiteral("\\pm"),    QStringLiteral("±"));
    s.replace(QStringLiteral("\\leq"),   QStringLiteral("≤"));
    s.replace(QStringLiteral("\\geq"),   QStringLiteral("≥"));
    s.replace(QStringLiteral("\\neq"),   QStringLiteral("≠"));
    s.replace(QStringLiteral("\\approx"),QStringLiteral("≈"));
    s.replace(QStringLiteral("\\infty"), QStringLiteral("∞"));
    s.replace(QStringLiteral("\\pi"),    QStringLiteral("π"));
    s.replace(QStringLiteral("\\alpha"), QStringLiteral("α"));
    s.replace(QStringLiteral("\\beta"),  QStringLiteral("β"));
    s.replace(QStringLiteral("\\gamma"), QStringLiteral("γ"));
    s.replace(QStringLiteral("\\delta"), QStringLiteral("δ"));
    s.replace(QStringLiteral("\\Delta"), QStringLiteral("Δ"));
    s.replace(QStringLiteral("\\theta"), QStringLiteral("θ"));
    s.replace(QStringLiteral("\\lambda"),QStringLiteral("λ"));
    s.replace(QStringLiteral("\\mu"),    QStringLiteral("μ"));
    s.replace(QStringLiteral("\\sigma"), QStringLiteral("σ"));
    s.replace(QStringLiteral("\\Sigma"), QStringLiteral("Σ"));
    s.replace(QStringLiteral("\\omega"), QStringLiteral("ω"));
    s.replace(QStringLiteral("\\cdot"),  QStringLiteral("·"));
    s.replace(QStringLiteral("\\ldots"), QStringLiteral("…"));
    s.replace(QStringLiteral("\\cdots"), QStringLiteral("⋯"));
    s.replace(QStringLiteral("\\%"), QStringLiteral("%"));
    s.replace(QStringLiteral("\\left"), QString());
    s.replace(QStringLiteral("\\right"), QString());
    // Text wrappers can be shown directly; preserve other commands so formulas remain inspectable.
    static const QRegularExpression textCmdRe(
        QStringLiteral("\\\\(?:text|mathrm|mathbf|operatorname)\\{([^}]*)\\}"));
    s.replace(textCmdRe, QStringLiteral("\\1"));
    static const QRegularExpression functionRe(
        QStringLiteral("\\\\(sin|cos|tan|log|ln)\\{([^{}]+)\\}"));
    s.replace(functionRe, QStringLiteral("\\1(\\2)"));
    static const QRegularExpression bareFunctionRe(
        QStringLiteral("\\\\(sin|cos|tan|log|ln)\\b"));
    s.replace(bareFunctionRe, QStringLiteral("\\1"));

    return s;
}

QString formatQuestionHtml(const QString& html) {
    // 题库字段已经是 HTML；只转换文字节点，避免改写图片路径和标签属性。
    static const QRegularExpression tagRe(QStringLiteral("<[^>]*>"));
    QString result;
    int cursor = 0;
    auto match = tagRe.globalMatch(html);
    while (match.hasNext()) {
        const auto tag = match.next();
        result += normaliseFormulas(html.mid(cursor, tag.capturedStart() - cursor));
        result += tag.captured();
        cursor = tag.capturedEnd();
    }
    result += normaliseFormulas(html.mid(cursor));
    return result;
}

}  // namespace quizpane
