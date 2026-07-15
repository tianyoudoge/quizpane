#include "quizpane/studio/option_label.hpp"

namespace quizpane::studio {

QString canonicalOptionLabel(const QString& raw) {
    const QString text = raw.normalized(QString::NormalizationForm_C).trimmed();
    if (text.isEmpty()) return {};
    if (text.size() == 1) {
        const ushort code = text.at(0).unicode();
        if ((code >= 'A' && code <= 'I') || (code >= 'a' && code <= 'i'))
            return text.at(0).toLower();
        if (code >= 0xFF21 && code <= 0xFF29) return QChar(code - 0xFF21 + u'a');
        if (code >= 0xFF41 && code <= 0xFF49) return QChar(code - 0xFF41 + u'a');
        if (code >= 0x2460 && code <= 0x2473) return QChar(u'a' + code - 0x2460);
        if (code >= 0x2488 && code <= 0x2490) return QChar(u'a' + code - 0x2488);
    }
    static const QRegularExpression numeric(
        QStringLiteral(R"(^[（(\[{「『]?\s*([1-9])\s*[)）\]}」』]?$)"));
    const auto match = numeric.match(text);
    return match.hasMatch() ? QString(QChar(u'a' + match.captured(1).toInt() - 1))
                            : QString{};
}

const QRegularExpression& optionLinePattern() {
    static const QRegularExpression pattern(QStringLiteral(
        R"((?:^|\n)\s*(?:[A-IＡ-Ｉa-iａ-ｉ]|[①-⑳⒈-⒐]|[（(\[{「『]?[1-9][)）\]}」』])\s*[.、．:：]?\s*\S)"));
    return pattern;
}

}  // namespace quizpane::studio
