#include "ai_response_format.hpp"

#include <QApplication>
#include <QPalette>
#include <QEvent>
#include <QRegularExpression>
#include <QStringList>

namespace quizpane::ui {
namespace {

struct MathPart {
    QString latex;
    bool display = false;
};

// 公式先替换成私用区占位符，Markdown 处理完再还原，避免 * _ 等字符被误当作强调语法。
QString token(int index) {
    return QChar(0xE000) + QString::number(index) + QChar(0xE001);
}

QString extractDelimitedMath(const QString& raw, QList<MathPart>* parts) {
    static const QRegularExpression re(QStringLiteral(
        "\\$\\$([\\s\\S]+?)\\$\\$|\\\\\\[([\\s\\S]+?)\\\\\\]|\\\\\\(([\\s\\S]+?)\\\\\\)|\\$([^$\\n]+?)\\$"));
    QString out;
    int last = 0;
    auto it = re.globalMatch(raw);
    while (it.hasNext()) {
        const auto m = it.next();
        out += raw.mid(last, m.capturedStart() - last);
        const bool display = m.capturedLength(1) > 0 || m.capturedLength(2) > 0;
        const QString latex = m.captured(1) + m.captured(2) + m.captured(3) + m.captured(4);
        parts->append({latex.trimmed(), display});
        out += token(static_cast<int>(parts->size()) - 1);
        last = m.capturedEnd();
    }
    return out + raw.mid(last);
}

QString codeToken(int index) {
    return QChar(0xE002) + QString::number(index) + QChar(0xE003);
}

// 先隔离代码块，保留空白与字面内容；代码里的 Markdown/公式不参与转换。
QString extractFencedCode(const QString& raw, QList<MathPart>* parts, QStringList* blocks) {
    static const QRegularExpression fence(QStringLiteral("^ {0,3}(`{3,}|~{3,})(.*)$"));
    QStringList prose, code;
    QString marker;
    QString out;
    const auto flushProse = [&] {
        out += extractDelimitedMath(prose.join(QLatin1Char('\n')), parts);
        prose.clear();
    };
    const auto flushCode = [&] {
        blocks->append(QStringLiteral("<pre>%1</pre>").arg(code.join(QLatin1Char('\n')).toHtmlEscaped()));
        out += codeToken(static_cast<int>(blocks->size()) - 1) + QLatin1Char('\n');
        code.clear();
    };
    for (const QString& line : raw.split(QLatin1Char('\n'))) {
        const auto m = fence.match(line);
        if (!marker.isEmpty()) {
            if (m.hasMatch() && m.captured(1).at(0) == marker.at(0) &&
                m.captured(1).size() >= marker.size() && m.captured(2).trimmed().isEmpty()) {
                flushCode();
                marker.clear();
            } else {
                code.append(line);
            }
        } else if (m.hasMatch() &&
                   (m.captured(1).at(0) != QLatin1Char('`') || !m.captured(2).contains(QLatin1Char('`')))) {
            flushProse();
            out += QLatin1Char('\n');
            marker = m.captured(1);
        } else {
            prose.append(line);
        }
    }
    if (!marker.isEmpty()) flushCode();
    flushProse();
    return out;
}

bool isBareMathChar(QChar c) {
    return c.unicode() > 32 && c.unicode() < 127 && c != QLatin1Char('<') && c != QLatin1Char('>');
}

bool startsBareMath(const QString& text, int i) {
    const QChar c = text.at(i);
    const QChar next = i + 1 < text.size() ? text.at(i + 1) : QChar();
    const QChar prev = i > 0 ? text.at(i - 1) : QChar();
    if (c == QLatin1Char('\\')) return next.isLetter() && next.unicode() < 128;
    if (c == QLatin1Char('^')) return prev.isLetterOrNumber() || prev == QLatin1Char(')');
    if (c == QLatin1Char('_')) return next == QLatin1Char('{');
    return false;
}

// 模型偶尔忘记写 $…$：把含 \frac、^{…} 等的连续片段（包括花括号里的中文）当作公式。
QString extractBareMath(const QString& text, QList<MathPart>* parts) {
    QString out;
    int last = 0;
    for (int i = 0; i < text.size(); ++i) {
        if (text.at(i).unicode() == 0xE000 || !startsBareMath(text, i)) continue;
        int start = i;
        while (start > last && isBareMathChar(text.at(start - 1))) --start;
        int end = i;
        while (end < text.size()) {
            const QChar c = text.at(end);
            if (c == QLatin1Char('{')) {
                int depth = 0;
                while (end < text.size()) {
                    if (text.at(end) == QLatin1Char('{')) ++depth;
                    else if (text.at(end) == QLatin1Char('}') && --depth == 0) break;
                    ++end;
                }
                if (end < text.size()) ++end;
            } else if (isBareMathChar(c)) {
                ++end;
            } else {
                break;
            }
        }
        // 句末的中英文标点不属于公式。
        while (end > i + 1 && QStringLiteral(",.;:").contains(text.at(end - 1))) --end;
        out += text.mid(last, start - last);
        parts->append({text.mid(start, end - start), false});
        out += token(static_cast<int>(parts->size()) - 1);
        last = end;
        i = end - 1;
    }
    return out + text.mid(last);
}

QString inlineMarkdown(QString text) {
    static const QRegularExpression bold(QStringLiteral("\\*\\*(.+?)\\*\\*"));
    static const QRegularExpression italic(
        QStringLiteral("(?<![*\\w])\\*(?![\\s*])(.+?)(?<![\\s*])\\*(?![*\\w])"));
    static const QRegularExpression code(QStringLiteral("`([^`\\n]+)`"));
    text.replace(bold, QStringLiteral("<b>\\1</b>"));
    text.replace(italic, QStringLiteral("<i>\\1</i>"));
    text.replace(code, QStringLiteral("<code>\\1</code>"));
    text.replace(QStringLiteral("\\%"), QStringLiteral("%"));
    return text;
}

}  // namespace

QString formatAiResponse(const QString& raw, const AiResponseStyle& style) {
    QList<MathPart> parts;
    QStringList codeBlocks;
    QString normalized = raw;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    QString text = extractFencedCode(normalized, &parts, &codeBlocks);
    // 兼容旧提示词下模型输出的 <br> 换行。
    static const QRegularExpression brTag(QStringLiteral("<br\\s*/?>"), QRegularExpression::CaseInsensitiveOption);
    text.replace(brTag, QStringLiteral("\n"));

    static const QRegularExpression header(QStringLiteral("^【(答案确认|解题步骤|知识点|速算技巧)】\\s*(.*)$"));
    static const QRegularExpression bullet(QStringLiteral("^(?:[-•]|\\*(?!\\*))\\s+(.*)$"));
    static const QRegularExpression heading(QStringLiteral("^#{1,6}\\s+(.*)$"));
    static const QRegularExpression displayOnly(QStringLiteral("^\\x{E000}(\\d+)\\x{E001}$"));
    static const QRegularExpression codeOnly(QStringLiteral("^\\x{E002}(\\d+)\\x{E003}$"));

    QString html;
    QStringList paragraph;
#if defined(Q_OS_WIN)
    constexpr int paragraphGap = 8;
    constexpr int sectionGap = 12;
    constexpr int listGap = 6;
#else
    constexpr int paragraphGap = 6;
    constexpr int sectionGap = 8;
    constexpr int listGap = 4;
#endif
    const auto flushParagraph = [&] {
        if (paragraph.isEmpty()) return;
        html += QStringLiteral("<p style=\"margin:0 0 %1px 0\">%2</p>")
                    .arg(paragraphGap).arg(paragraph.join(QStringLiteral("<br>")));
        paragraph.clear();
    };

    for (const QString& rawLine : text.split(QLatin1Char('\n'))) {
        if (codeOnly.match(rawLine).hasMatch()) {
            flushParagraph();
            html += rawLine;
            continue;
        }
        const QString line = extractBareMath(rawLine.trimmed(), &parts);
        if (line.isEmpty()) {
            flushParagraph();
            continue;
        }
        if (const auto m = header.match(line); m.hasMatch()) {
            flushParagraph();
            const QColor color = m.captured(1) == QStringLiteral("速算技巧") ? style.tipAccent : style.accent;
            html += QStringLiteral("<p style=\"margin:%3px 0 4px 0\"><b style=\"color:%1\">【%2】</b></p>")
                        .arg(color.name(), m.captured(1)).arg(sectionGap);
            if (!m.captured(2).isEmpty()) paragraph << inlineMarkdown(m.captured(2));
            continue;
        }
        if (const auto m = displayOnly.match(line);
            m.hasMatch() && parts.at(m.captured(1).toInt()).display) {
            flushParagraph();
            html += line;
            continue;
        }
        if (const auto m = bullet.match(line); m.hasMatch()) {
            // Qt 的 <li> 圆点贴着行顶，遇到公式图片会和文字错开；改为行内圆点加悬挂缩进。
            flushParagraph();
            html += QStringLiteral("<p style=\"margin:0 0 %1px 14px; text-indent:-11px\">•&nbsp;%2</p>")
                        .arg(listGap).arg(inlineMarkdown(m.captured(1)));
            continue;
        }
        if (const auto m = heading.match(line); m.hasMatch()) {
            flushParagraph();
            html += QStringLiteral("<p style=\"margin:6px 0 4px 0\"><b>%1</b></p>").arg(inlineMarkdown(m.captured(1)));
            continue;
        }
        paragraph << inlineMarkdown(line);
    }
    flushParagraph();

    for (int i = static_cast<int>(parts.size()) - 1; i >= 0; --i) {
        const MathPart& part = parts.at(i);
        const QString rendered = mathHtml(part.latex, style.math, part.display);
        html.replace(token(i), part.display
            ? QStringLiteral("<div align=\"center\" style=\"margin:4px 0\">%1</div>").arg(rendered)
            : rendered);
    }
    for (int i = static_cast<int>(codeBlocks.size()) - 1; i >= 0; --i)
        html.replace(codeToken(i), codeBlocks.at(i));
    return QStringLiteral("<div>%1</div>").arg(html);
}


QString formatQuestionHtml(const QString& html) {
    MathStyle style;
    style.font = QApplication::font();
    style.color = QApplication::palette().color(QPalette::Text);
    style.devicePixelRatio = 1.0;
    // 只转换文字节点，不改写图片路径和标签属性。
    static const QRegularExpression tagRe(QStringLiteral("<[^>]*>"));
    static const QRegularExpression mathRe(QStringLiteral("\\$([^$\\n]+?)\\$"));
    QString result;
    int cursor = 0;
    auto match = tagRe.globalMatch(html);
    while (match.hasNext()) {
        const auto tag = match.next();
        QString segment = html.mid(cursor, tag.capturedStart() - cursor);
        // 把行内 $...$ 转成公式图片，其余部分原样保留
        QStringList parts;
        int last = 0;
        auto mi = mathRe.globalMatch(segment);
        while (mi.hasNext()) {
            const auto m = mi.next();
            parts << segment.mid(last, m.capturedStart() - last).toHtmlEscaped();
            parts << mathHtml(m.captured(1), style, false);
            last = m.capturedEnd();
        }
        parts << segment.mid(last);
        result += parts.join(QString());
        result += tag.captured();
        cursor = tag.capturedEnd();
    }
    QString segment = html.mid(cursor);
    QStringList parts;
    int last = 0;
    auto mi = mathRe.globalMatch(segment);
    while (mi.hasNext()) {
        const auto m = mi.next();
        parts << segment.mid(last, m.capturedStart() - last).toHtmlEscaped();
        parts << mathHtml(m.captured(1), style, false);
        last = m.capturedEnd();
    }
    parts << segment.mid(last);
    result += parts.join(QString());
    return result;
}

AiResponseLabel::AiResponseLabel(QWidget* parent) : QLabel(parent) {
    setWordWrap(true);
    setTextFormat(Qt::RichText);
    setOpenExternalLinks(false);
    setTextInteractionFlags(Qt::TextSelectableByMouse);
}

void AiResponseLabel::setResponse(const QString& raw) {
    raw_ = raw;
    render();
}

void AiResponseLabel::setMessage(const QString& html) {
    raw_.clear();
    setText(html);
}

void AiResponseLabel::changeEvent(QEvent* event) {
    QLabel::changeEvent(event);
    switch (event->type()) {
    case QEvent::FontChange:
    case QEvent::PaletteChange:
    case QEvent::StyleChange:
        if (!raw_.isEmpty() && !rendering_) render();
        break;
    default:
        break;
    }
}

void AiResponseLabel::render() {
    rendering_ = true;
    AiResponseStyle style;
    style.math.font = font();
    style.math.color = palette().color(foregroundRole());
    style.math.devicePixelRatio = devicePixelRatioF();
    style.math.maxWidth = width() > 40 ? width() - 4 : 0;
    const bool darkText = style.math.color.lightnessF() < 0.5;
    style.accent = darkText ? QColor(0x2d, 0x6b, 0x4f) : QColor(0x9f, 0xc4, 0xb0);
    style.tipAccent = darkText ? QColor(0x9a, 0x6b, 0x1f) : QColor(0xc8, 0xa9, 0x6e);
    setText(formatAiResponse(raw_, style));
    rendering_ = false;
}

}  // namespace quizpane::ui
