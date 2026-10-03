#include "math_formula.hpp"

#include <QBuffer>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace quizpane::ui {
namespace {

// TeX 原子类别，只用于决定运算符两侧的间距。
enum class Cls { Ord, Bin, Rel, Open, Close, Punct, Op };

struct Box {
    virtual ~Box() = default;
    virtual void draw(QPainter& painter, qreal x, qreal baseline) const = 0;
    // 二维结构（分数、根号、拉伸括号等）无法用 Qt 富文本表达，需要画成图片。
    virtual bool needsImage() const { return false; }
    virtual QString html() const = 0;
    qreal width = 0;
    qreal ascent = 0;
    qreal descent = 0;
    Cls cls = Cls::Ord;
};
using BoxPtr = std::unique_ptr<Box>;

struct Context {
    QFont uiFont;
    QFont mathFont;
    qreal px = 14;
    qreal thickness = 1;  // 分数线与根号笔画粗细，至少一个设备像素
};

QFont scaledFont(QFont font, qreal px) {
    font.setPixelSize(std::max(7, qRound(px)));
    return font;
}

struct TextBox final : Box {
    TextBox(QString text, QFont font, bool italic, Cls atomClass) : text(std::move(text)), font(std::move(font)), italic(italic) {
        cls = atomClass;
        this->font.setItalic(italic);
        const QFontMetricsF fm(this->font);
        advance = fm.horizontalAdvance(this->text);
        // 斜体字母右侧会探出字框，补一点“斜体校正”，避免与上标或括号相撞。
        if (italic) advance += fm.height() * 0.06;
        const QRectF ink = fm.tightBoundingRect(this->text);
        ascent = std::max(-ink.top(), fm.xHeight());
        descent = std::max(ink.bottom(), 0.0);
        width = advance;
    }
    void setPads(qreal left, qreal right) {
        leftPad = left;
        rightPad = right;
        width = leftPad + advance + rightPad;
    }
    void draw(QPainter& painter, qreal x, qreal baseline) const override {
        painter.setFont(font);
        painter.drawText(QPointF(x + leftPad, baseline), text);
    }
    QString html() const override {
        QString body = text.toHtmlEscaped();
        if (italic) body = QStringLiteral("<i style=\"font-family:'Times New Roman',serif\">%1</i>").arg(body);
        const QString thin = QString(QChar(0x2009));
        return (leftPad > 0 ? thin : QString()) + body + (rightPad > 0 ? thin : QString());
    }
    QString text;
    QFont font;
    bool italic = false;
    qreal advance = 0;
    qreal leftPad = 0;
    qreal rightPad = 0;
};

struct SpaceBox final : Box {
    explicit SpaceBox(qreal w) { width = w; }
    void draw(QPainter&, qreal, qreal) const override {}
    QString html() const override { return width > 0 ? QString(QChar(0x2009)) : QString(); }
};

struct RowBox final : Box {
    explicit RowBox(std::vector<BoxPtr> items) : children(std::move(items)) { measure(); }
    void measure() {
        width = ascent = descent = 0;
        for (const BoxPtr& child : children) {
            width += child->width;
            ascent = std::max(ascent, child->ascent);
            descent = std::max(descent, child->descent);
        }
    }
    void draw(QPainter& painter, qreal x, qreal baseline) const override {
        for (const BoxPtr& child : children) {
            child->draw(painter, x, baseline);
            x += child->width;
        }
    }
    bool needsImage() const override {
        return std::any_of(children.begin(), children.end(),
                           [](const BoxPtr& child) { return child->needsImage(); });
    }
    QString html() const override {
        QString out;
        for (const BoxPtr& child : children) out += child->html();
        return out;
    }
    std::vector<BoxPtr> children;
};

struct FracBox final : Box {
    FracBox(BoxPtr numerator, BoxPtr denominator, const Context& ctx, qreal scale, bool display)
        : num(std::move(numerator)), den(std::move(denominator)) {
        const qreal em = ctx.px * scale;
        thickness = ctx.thickness;
        axis = em * 0.27;
        const qreal gap = std::max(1.5, em * (display ? 0.16 : 0.11));
        pad = em * 0.12;
        numBaseline = axis + thickness / 2 + gap + num->descent;
        denBaseline = axis - thickness / 2 - gap - den->ascent;
        width = std::max(num->width, den->width) + 2 * pad;
        ascent = numBaseline + num->ascent;
        descent = den->descent - denBaseline;
    }
    void draw(QPainter& painter, qreal x, qreal baseline) const override {
        num->draw(painter, x + (width - num->width) / 2, baseline - numBaseline);
        den->draw(painter, x + (width - den->width) / 2, baseline - denBaseline);
        painter.fillRect(QRectF(x + pad / 2, baseline - axis - thickness / 2, width - pad, thickness),
                         painter.pen().color());
    }
    bool needsImage() const override { return true; }
    QString html() const override { return num->html() + QStringLiteral("/") + den->html(); }
    BoxPtr num, den;
    qreal thickness = 1, axis = 0, pad = 0, numBaseline = 0, denBaseline = 0;
};

struct SqrtBox final : Box {
    SqrtBox(BoxPtr radicand, BoxPtr rootIndex, const Context& ctx, qreal scale)
        : body(std::move(radicand)), index(std::move(rootIndex)) {
        const qreal em = ctx.px * scale;
        thickness = ctx.thickness;
        gap = std::max(1.5, em * 0.12);
        const qreal inner = body->ascent + gap + body->descent;
        signWidth = std::max(em * 0.55, inner * 0.3);
        indexShift = index ? std::max(0.0, index->width - signWidth * 0.45) : 0;
        width = indexShift + signWidth + body->width + em * 0.1;
        ascent = body->ascent + gap + thickness;
        descent = body->descent + em * 0.06;
        if (index) ascent = std::max(ascent, inner * 0.6 - body->descent + index->ascent + index->descent);
    }
    void draw(QPainter& painter, qreal x, qreal baseline) const override {
        const qreal top = baseline - body->ascent - gap - thickness / 2;
        const qreal bottom = baseline + descent;
        const qreal left = x + indexShift;
        const qreal h = bottom - top;
        QPainterPath path;
        path.moveTo(left, bottom - h * 0.42);
        path.lineTo(left + signWidth * 0.25, bottom - h * 0.5);
        path.lineTo(left + signWidth * 0.55, bottom);
        path.lineTo(left + signWidth, top);
        path.lineTo(x + width, top);
        QPen pen = painter.pen();
        pen.setWidthF(thickness);
        pen.setJoinStyle(Qt::MiterJoin);
        painter.save();
        painter.setPen(pen);
        painter.drawPath(path);
        painter.restore();
        body->draw(painter, left + signWidth, baseline);
        if (index) index->draw(painter, x, bottom - h * 0.55 - index->descent);
    }
    bool needsImage() const override { return true; }
    QString html() const override { return QStringLiteral("√(") + body->html() + QStringLiteral(")"); }
    BoxPtr body, index;
    qreal thickness = 1, gap = 0, signWidth = 0, indexShift = 0;
};

struct OverlineBox final : Box {
    OverlineBox(BoxPtr content, const Context& ctx, qreal scale) : body(std::move(content)) {
        thickness = ctx.thickness;
        gap = std::max(1.5, ctx.px * scale * 0.1);
        width = body->width;
        ascent = body->ascent + gap + thickness;
        descent = body->descent;
    }
    void draw(QPainter& painter, qreal x, qreal baseline) const override {
        body->draw(painter, x, baseline);
        painter.fillRect(QRectF(x, baseline - body->ascent - gap - thickness, width, thickness),
                         painter.pen().color());
    }
    bool needsImage() const override { return true; }
    QString html() const override {
        return QStringLiteral("<span style=\"text-decoration:overline\">%1</span>").arg(body->html());
    }
    BoxPtr body;
    qreal thickness = 1, gap = 0;
};

struct ScriptsBox final : Box {
    ScriptsBox(BoxPtr basePart, BoxPtr superscript, BoxPtr subscript, const Context& ctx, qreal scale)
        : base(std::move(basePart)), sup(std::move(superscript)), sub(std::move(subscript)) {
        const qreal em = ctx.px * scale;
        const qreal baseWidth = base ? base->width : 0;
        const qreal baseAscent = base ? base->ascent : em * 0.45;
        const qreal baseDescent = base ? base->descent : 0;
        cls = base ? base->cls : Cls::Ord;
        if (sup) supShift = std::max(em * 0.38, baseAscent - sup->ascent * 0.5);
        if (sub) subShift = std::max(em * 0.22, baseDescent + sub->ascent * 0.3);
        if (sup && sub) {
            const qreal clearance = (supShift - sup->descent) - (sub->ascent - subShift);
            if (clearance < em * 0.15) subShift += em * 0.15 - clearance;
        }
        width = baseWidth + std::max(sup ? sup->width : 0, sub ? sub->width : 0) + em * 0.04;
        ascent = std::max(baseAscent, sup ? supShift + sup->ascent : 0);
        descent = std::max(baseDescent, sub ? subShift + sub->descent : 0);
    }
    void draw(QPainter& painter, qreal x, qreal baseline) const override {
        qreal scriptX = x;
        if (base) {
            base->draw(painter, x, baseline);
            scriptX += base->width;
        }
        if (sup) sup->draw(painter, scriptX, baseline - supShift);
        if (sub) sub->draw(painter, scriptX, baseline + subShift);
    }
    bool needsImage() const override {
        return (base && base->needsImage()) || (sup && sup->needsImage()) || (sub && sub->needsImage());
    }
    QString html() const override {
        QString out = base ? base->html() : QString();
        if (sup) out += QStringLiteral("<sup>%1</sup>").arg(sup->html());
        if (sub) out += QStringLiteral("<sub>%1</sub>").arg(sub->html());
        return out;
    }
    BoxPtr base, sup, sub;
    qreal supShift = 0, subShift = 0;
};

// 按内容高度拉伸的定界符：\left( … \right)，以及包住分数等高内容的普通括号。
struct DelimBox final : Box {
    DelimBox(QString openDelim, QString closeDelim, BoxPtr content, const Context& ctx, qreal scale)
        : open(std::move(openDelim)), close(std::move(closeDelim)), body(std::move(content)) {
        const qreal em = ctx.px * scale;
        font = scaledFont(ctx.mathFont, em);
        const QFontMetricsF fm(font);
        const qreal normal = fm.capHeight() + fm.descent();
        stretch = body->ascent + body->descent > normal * 1.15;
        if (stretch) {
            const qreal extra = em * 0.08;
            ascent = body->ascent + extra;
            descent = body->descent + extra;
            delimWidth = std::max(em * 0.35, (ascent + descent) * 0.12);
            stroke = std::max(ctx.thickness, em * 0.06);
        } else {
            const QRectF ink = fm.tightBoundingRect(open + close);
            ascent = std::max(body->ascent, -ink.top());
            descent = std::max(body->descent, ink.bottom());
        }
        openWidth = open.isEmpty() ? 0 : (stretch ? delimWidth : fm.horizontalAdvance(open));
        closeWidth = close.isEmpty() ? 0 : (stretch ? delimWidth : fm.horizontalAdvance(close));
        width = openWidth + body->width + closeWidth;
    }
    void drawDelim(QPainter& painter, const QString& delim, qreal x, qreal w, qreal baseline,
                   bool left) const {
        if (delim.isEmpty()) return;
        if (!stretch) {
            painter.setFont(font);
            painter.drawText(QPointF(x, baseline), delim);
            return;
        }
        const qreal top = baseline - ascent;
        const qreal bottom = baseline + descent;
        const qreal mid = (top + bottom) / 2;
        const qreal inner = left ? x + w * 0.8 : x + w * 0.2;
        const qreal outer = left ? x + w * 0.15 : x + w * 0.85;
        QPainterPath path;
        if (delim == QStringLiteral("(") || delim == QStringLiteral(")")) {
            path.moveTo(inner, top);
            path.quadTo(outer - (inner - outer) * 0.35, mid, inner, bottom);
        } else if (delim == QStringLiteral("[") || delim == QStringLiteral("]")) {
            path.moveTo(inner, top);
            path.lineTo(outer, top);
            path.lineTo(outer, bottom);
            path.lineTo(inner, bottom);
        } else if (delim == QStringLiteral("|")) {
            path.moveTo(x + w / 2, top);
            path.lineTo(x + w / 2, bottom);
        } else if (delim == QStringLiteral("‖")) {
            path.moveTo(x + w * 0.35, top);
            path.lineTo(x + w * 0.35, bottom);
            path.moveTo(x + w * 0.65, top);
            path.lineTo(x + w * 0.65, bottom);
        } else {
            // { } 等字形没有简单的笔画表示，按高度纵向拉伸原字形。
            painter.save();
            painter.setFont(font);
            const QFontMetricsF fm(font);
            const QRectF ink = fm.tightBoundingRect(delim);
            const qreal factor = (bottom - top) / std::max(1.0, ink.height());
            painter.translate(x + (w - ink.width()) / 2 - ink.left(), top - ink.top() * factor);
            painter.scale(1.0, factor);
            painter.drawText(QPointF(0, 0), delim);
            painter.restore();
            return;
        }
        QPen pen = painter.pen();
        pen.setWidthF(stroke);
        pen.setCapStyle(Qt::FlatCap);
        painter.save();
        painter.setPen(pen);
        painter.drawPath(path);
        painter.restore();
    }
    void draw(QPainter& painter, qreal x, qreal baseline) const override {
        drawDelim(painter, open, x, openWidth, baseline, true);
        body->draw(painter, x + openWidth, baseline);
        drawDelim(painter, close, x + openWidth + body->width, closeWidth, baseline, false);
    }
    bool needsImage() const override { return stretch || body->needsImage(); }
    QString html() const override { return open.toHtmlEscaped() + body->html() + close.toHtmlEscaped(); }
    QString open, close;
    BoxPtr body;
    QFont font;
    bool stretch = false;
    qreal delimWidth = 0, openWidth = 0, closeWidth = 0, stroke = 1;
};

struct Symbol {
    QString text;
    Cls cls;
};

const QHash<QString, Symbol>& symbols() {
    static const QHash<QString, Symbol> table = [] {
        QHash<QString, Symbol> t;
        const auto add = [&](const char* names, const char* text, Cls cls) {
            for (const QString& name : QString::fromLatin1(names).split(QLatin1Char(' ')))
                t.insert(name, {QString::fromUtf8(text), cls});
        };
        add("times", "×", Cls::Bin);
        add("div", "÷", Cls::Bin);
        add("cdot", "·", Cls::Bin);
        add("pm", "±", Cls::Bin);
        add("mp", "∓", Cls::Bin);
        add("ast", "∗", Cls::Bin);
        add("cap", "∩", Cls::Bin);
        add("cup", "∪", Cls::Bin);
        add("circ", "∘", Cls::Bin);
        add("leq le", "≤", Cls::Rel);
        add("geq ge", "≥", Cls::Rel);
        add("leqslant", "⩽", Cls::Rel);
        add("geqslant", "⩾", Cls::Rel);
        add("neq ne", "≠", Cls::Rel);
        add("approx", "≈", Cls::Rel);
        add("sim", "∼", Cls::Rel);
        add("equiv", "≡", Cls::Rel);
        add("propto", "∝", Cls::Rel);
        add("lt", "<", Cls::Rel);
        add("gt", ">", Cls::Rel);
        add("ll", "≪", Cls::Rel);
        add("gg", "≫", Cls::Rel);
        add("to rightarrow", "→", Cls::Rel);
        add("leftarrow", "←", Cls::Rel);
        add("Rightarrow implies", "⇒", Cls::Rel);
        add("Leftrightarrow iff", "⇔", Cls::Rel);
        add("in", "∈", Cls::Rel);
        add("notin", "∉", Cls::Rel);
        add("subset", "⊂", Cls::Rel);
        add("subseteq", "⊆", Cls::Rel);
        add("perp", "⊥", Cls::Rel);
        add("parallel", "∥", Cls::Rel);
        add("therefore", "∴", Cls::Rel);
        add("because", "∵", Cls::Rel);
        add("infty", "∞", Cls::Ord);
        add("partial", "∂", Cls::Ord);
        add("angle", "∠", Cls::Ord);
        add("triangle", "△", Cls::Ord);
        add("degree", "°", Cls::Ord);
        add("prime", "′", Cls::Ord);
        add("ldots dots", "…", Cls::Ord);
        add("cdots", "⋯", Cls::Ord);
        add("alpha", "α", Cls::Ord);
        add("beta", "β", Cls::Ord);
        add("gamma", "γ", Cls::Ord);
        add("delta", "δ", Cls::Ord);
        add("epsilon varepsilon", "ε", Cls::Ord);
        add("theta", "θ", Cls::Ord);
        add("lambda", "λ", Cls::Ord);
        add("mu", "μ", Cls::Ord);
        add("pi", "π", Cls::Ord);
        add("rho", "ρ", Cls::Ord);
        add("sigma", "σ", Cls::Ord);
        add("tau", "τ", Cls::Ord);
        add("phi varphi", "φ", Cls::Ord);
        add("omega", "ω", Cls::Ord);
        add("Gamma", "Γ", Cls::Ord);
        add("Delta", "Δ", Cls::Ord);
        add("Theta", "Θ", Cls::Ord);
        add("Lambda", "Λ", Cls::Ord);
        add("Pi", "Π", Cls::Ord);
        add("Sigma", "Σ", Cls::Ord);
        add("Phi", "Φ", Cls::Ord);
        add("Omega", "Ω", Cls::Ord);
        add("langle", "⟨", Cls::Open);
        add("rangle", "⟩", Cls::Close);
        add("lfloor", "⌊", Cls::Open);
        add("rfloor", "⌋", Cls::Close);
        add("lceil", "⌈", Cls::Open);
        add("rceil", "⌉", Cls::Close);
        return t;
    }();
    return table;
}

bool isFunctionName(const QString& name) {
    static const QStringList names{
        QStringLiteral("sin"), QStringLiteral("cos"), QStringLiteral("tan"), QStringLiteral("cot"),
        QStringLiteral("sec"), QStringLiteral("csc"), QStringLiteral("log"), QStringLiteral("ln"),
        QStringLiteral("lg"), QStringLiteral("exp"), QStringLiteral("lim"), QStringLiteral("max"),
        QStringLiteral("min"), QStringLiteral("arg"), QStringLiteral("det"), QStringLiteral("gcd"),
        QStringLiteral("arcsin"), QStringLiteral("arccos"), QStringLiteral("arctan")};
    return names.contains(name);
}

bool isCjkOrText(QChar c) {
    return c.unicode() >= 0x2E80 || (c.unicode() >= 0x3000 && c.unicode() <= 0x30FF);
}

class Parser {
public:
    Parser(QString source, const Context& ctx) : src_(std::move(source)), ctx_(ctx) {}

    BoxPtr parse(bool display) { return parseRow(1.0, display, Stop::None); }

private:
    enum class Stop { None, Brace, Bracket, Right };

    QChar peek(int offset = 0) const {
        const int i = pos_ + offset;
        return i < src_.size() ? src_.at(i) : QChar();
    }
    bool atEnd() const { return pos_ >= src_.size(); }
    void skipSpaces() {
        while (!atEnd() && peek().isSpace()) ++pos_;
    }
    bool lookingAtCommand(const QString& name) const {
        if (src_.mid(pos_, name.size() + 1) != QLatin1Char('\\') + name) return false;
        const QChar after = pos_ + name.size() + 1 < src_.size() ? src_.at(pos_ + name.size() + 1) : QChar();
        return !after.isLetter();
    }
    QString readCommandName() {
        // 调用时 pos_ 指向反斜杠。
        ++pos_;
        const int start = pos_;
        while (!atEnd() && peek().isLetter() && peek().unicode() < 128) ++pos_;
        if (pos_ == start && !atEnd()) ++pos_;  // 控制符号，如 \, \% \{
        return src_.mid(start, pos_ - start);
    }
    QString readRawGroup() {
        skipSpaces();
        if (peek() != QLatin1Char('{')) {
            if (atEnd()) return {};
            return QString(src_.at(pos_++));
        }
        int depth = 0;
        const int start = ++pos_;
        while (!atEnd()) {
            const QChar c = src_.at(pos_);
            if (c == QLatin1Char('\\') && pos_ + 1 < src_.size()) { pos_ += 2; continue; }
            if (c == QLatin1Char('{')) ++depth;
            if (c == QLatin1Char('}') && depth-- == 0) break;
            ++pos_;
        }
        const QString text = src_.mid(start, pos_ - start);
        if (!atEnd()) ++pos_;
        return text;
    }
    QString readDelimiter() {
        skipSpaces();
        if (atEnd()) return {};
        if (peek() != QLatin1Char('\\')) return QString(src_.at(pos_++));
        const QString name = readCommandName();
        if (name == QStringLiteral("{") || name == QStringLiteral("lbrace")) return QStringLiteral("{");
        if (name == QStringLiteral("}") || name == QStringLiteral("rbrace")) return QStringLiteral("}");
        if (name == QStringLiteral("|") || name == QStringLiteral("Vert")) return QStringLiteral("‖");
        if (name == QStringLiteral("vert")) return QStringLiteral("|");
        if (const auto it = symbols().constFind(name); it != symbols().constEnd()) return it->text;
        return {};
    }

    BoxPtr text(const QString& s, bool italic, Cls cls, qreal scale, bool math = true) const {
        return std::make_unique<TextBox>(s, scaledFont(math ? ctx_.mathFont : ctx_.uiFont, ctx_.px * scale),
                                         italic, cls);
    }
    BoxPtr plainText(const QString& s, qreal scale, bool bold = false) const {
        QFont font = scaledFont(ctx_.uiFont, ctx_.px * scale);
        font.setBold(bold);
        return std::make_unique<TextBox>(s, font, false, Cls::Ord);
    }

    BoxPtr parseArg(qreal scale, bool display) {
        skipSpaces();
        if (atEnd()) return std::make_unique<RowBox>(std::vector<BoxPtr>{});
        if (peek() == QLatin1Char('{')) {
            ++pos_;
            BoxPtr row = parseRow(scale, display, Stop::Brace);
            if (peek() == QLatin1Char('}')) ++pos_;
            return row;
        }
        BoxPtr atom = parseAtom(scale, display);
        return atom ? std::move(atom) : std::make_unique<RowBox>(std::vector<BoxPtr>{});
    }

    BoxPtr parseRow(qreal scale, bool display, Stop stop) {
        std::vector<BoxPtr> items;
        if (++depth_ > 40) {
            // 异常深的嵌套直接按源码显示，避免递归失控。
            items.push_back(plainText(src_.mid(pos_), scale));
            pos_ = static_cast<int>(src_.size());
        }
        while (!atEnd()) {
            skipSpaces();
            if (atEnd()) break;
            const QChar c = peek();
            if (stop == Stop::Brace && c == QLatin1Char('}')) break;
            if (stop == Stop::Bracket && c == QLatin1Char(']')) break;
            if (stop == Stop::Right && lookingAtCommand(QStringLiteral("right"))) break;
            if (c == QLatin1Char('^') || c == QLatin1Char('_')) {
                ++pos_;
                BoxPtr script = parseArg(std::max(0.55, scale * 0.72), false);
                attachScript(items, c == QLatin1Char('^'), std::move(script), scale);
                continue;
            }
            const int before = pos_;
            if (BoxPtr atom = parseAtom(scale, display)) {
                items.push_back(std::move(atom));
            } else if (pos_ == before) {
                items.push_back(text(QString(src_.at(pos_++)), false, Cls::Ord, scale));
            }
        }
        --depth_;
        applySpacing(items, scale);
        stretchParentheses(items, scale);
        if (items.size() == 1) return std::move(items.front());
        return std::make_unique<RowBox>(std::move(items));
    }

    void attachScript(std::vector<BoxPtr>& items, bool superscript, BoxPtr script, qreal scale) {
        if (!items.empty()) {
            if (auto* existing = dynamic_cast<ScriptsBox*>(items.back().get())) {
                BoxPtr& slot = superscript ? existing->sup : existing->sub;
                if (!slot) {
                    BoxPtr base = std::move(existing->base);
                    BoxPtr sup = std::move(existing->sup);
                    BoxPtr sub = std::move(existing->sub);
                    (superscript ? sup : sub) = std::move(script);
                    items.back() = std::make_unique<ScriptsBox>(std::move(base), std::move(sup),
                                                                std::move(sub), ctx_, scale);
                    return;
                }
            }
        }
        BoxPtr base;
        if (!items.empty()) {
            base = std::move(items.back());
            items.pop_back();
        }
        items.push_back(superscript
            ? std::make_unique<ScriptsBox>(std::move(base), std::move(script), nullptr, ctx_, scale)
            : std::make_unique<ScriptsBox>(std::move(base), nullptr, std::move(script), ctx_, scale));
    }

    void applySpacing(std::vector<BoxPtr>& items, qreal scale) const {
        const qreal em = ctx_.px * scale;
        Cls previous = Cls::Open;  // 行首的 + − 视为正负号，不加二元运算间距
        for (BoxPtr& item : items) {
            auto* box = dynamic_cast<TextBox*>(item.get());
            if (box) {
                box->setPads(0, 0);
                if (box->cls == Cls::Bin &&
                    (previous == Cls::Bin || previous == Cls::Rel || previous == Cls::Open ||
                     previous == Cls::Punct || previous == Cls::Op)) {
                    box->cls = Cls::Ord;
                }
                // 上下标层级不加运算符间距（同 TeX 的 script style），分子分母略收窄。
                const qreal factor = scale < 0.8 ? 0.0 : scale < 0.95 ? 0.6 : 1.0;
                if (box->cls == Cls::Bin) box->setPads(em * 0.2 * factor, em * 0.2 * factor);
                else if (box->cls == Cls::Rel) box->setPads(em * 0.26 * factor, em * 0.26 * factor);
                else if (box->cls == Cls::Punct) box->setPads(0, em * 0.16);
                else if (box->cls == Cls::Op) box->setPads(0, em * 0.12);
            }
            if (!dynamic_cast<SpaceBox*>(item.get())) previous = item->cls;
        }
        // 行尾残留的二元运算符没有右操作数，同样去掉间距。
        if (!items.empty()) {
            if (auto* last = dynamic_cast<TextBox*>(items.back().get()); last && last->cls == Cls::Bin)
                last->setPads(last->leftPad, 0);
        }
    }

    static bool isParen(const BoxPtr& item, const char* chars, Cls cls) {
        const auto* box = dynamic_cast<const TextBox*>(item.get());
        return box && box->cls == cls && box->text.size() == 1 &&
               QByteArray(chars).contains(box->text.at(0).toLatin1());
    }

    void stretchParentheses(std::vector<BoxPtr>& items, qreal scale) const {
        std::vector<size_t> opens;
        for (size_t i = 0; i < items.size(); ++i) {
            if (isParen(items[i], "([", Cls::Open)) {
                opens.push_back(i);
                continue;
            }
            if (!isParen(items[i], ")]", Cls::Close) || opens.empty()) continue;
            const size_t open = opens.back();
            opens.pop_back();
            bool tall = false;
            for (size_t k = open + 1; k < i; ++k) tall = tall || items[k]->needsImage();
            if (!tall) continue;
            std::vector<BoxPtr> inner;
            for (size_t k = open + 1; k < i; ++k) inner.push_back(std::move(items[k]));
            const QString openText = static_cast<TextBox*>(items[open].get())->text;
            const QString closeText = static_cast<TextBox*>(items[i].get())->text;
            BoxPtr delim = std::make_unique<DelimBox>(openText, closeText,
                                                      std::make_unique<RowBox>(std::move(inner)), ctx_, scale);
            items.erase(items.begin() + static_cast<std::ptrdiff_t>(open) + 1,
                        items.begin() + static_cast<std::ptrdiff_t>(i) + 1);
            items[open] = std::move(delim);
            i = open;
        }
    }

    BoxPtr parseCommand(qreal scale, bool display) {
        const int commandStart = pos_;
        const QString name = readCommandName();
        const qreal em = ctx_.px * scale;
        if (name == QStringLiteral(",")) return std::make_unique<SpaceBox>(em * 0.17);
        if (name == QStringLiteral(":") || name == QStringLiteral(">")) return std::make_unique<SpaceBox>(em * 0.22);
        if (name == QStringLiteral(";")) return std::make_unique<SpaceBox>(em * 0.28);
        if (name == QStringLiteral(" ") || name == QStringLiteral("~")) return std::make_unique<SpaceBox>(em * 0.3);
        if (name == QStringLiteral("!")) return std::make_unique<SpaceBox>(0);
        if (name == QStringLiteral("quad")) return std::make_unique<SpaceBox>(em);
        if (name == QStringLiteral("qquad")) return std::make_unique<SpaceBox>(em * 2);
        if (name == QStringLiteral("\\")) return std::make_unique<SpaceBox>(em * 0.6);
        if (name == QStringLiteral("%") || name == QStringLiteral("$") || name == QStringLiteral("#") ||
            name == QStringLiteral("&") || name == QStringLiteral("_")) {
            return text(name, false, Cls::Ord, scale);
        }
        if (name == QStringLiteral("{")) return text(QStringLiteral("{"), false, Cls::Open, scale);
        if (name == QStringLiteral("}")) return text(QStringLiteral("}"), false, Cls::Close, scale);
        if (name == QStringLiteral("|")) return text(QStringLiteral("‖"), false, Cls::Ord, scale);
        if (name == QStringLiteral("frac") || name == QStringLiteral("dfrac") ||
            name == QStringLiteral("tfrac") || name == QStringLiteral("cfrac")) {
            // 行内分数的分子分母略缩小；独立成行的公式保持正文大小，更易读。
            const bool full = (display && name != QStringLiteral("tfrac")) || name == QStringLiteral("dfrac");
            const qreal partScale = full ? scale : std::max(0.6, scale * 0.85);
            BoxPtr num = parseArg(partScale, false);
            BoxPtr den = parseArg(partScale, false);
            return std::make_unique<FracBox>(std::move(num), std::move(den), ctx_, scale, full);
        }
        if (name == QStringLiteral("sqrt")) {
            BoxPtr index;
            skipSpaces();
            if (peek() == QLatin1Char('[')) {
                ++pos_;
                index = parseRow(std::max(0.55, scale * 0.6), false, Stop::Bracket);
                if (peek() == QLatin1Char(']')) ++pos_;
            }
            BoxPtr body = parseArg(scale, false);
            return std::make_unique<SqrtBox>(std::move(body), std::move(index), ctx_, scale);
        }
        if (name == QStringLiteral("left")) {
            const QString open = readDelimiter();
            BoxPtr body = parseRow(scale, display, Stop::Right);
            QString close;
            if (lookingAtCommand(QStringLiteral("right"))) {
                readCommandName();
                close = readDelimiter();
            }
            return std::make_unique<DelimBox>(open == QStringLiteral(".") ? QString() : open,
                                              close == QStringLiteral(".") ? QString() : close,
                                              std::move(body), ctx_, scale);
        }
        if (name == QStringLiteral("right")) return text(readDelimiter(), false, Cls::Close, scale);
        if (name == QStringLiteral("big") || name == QStringLiteral("Big") || name == QStringLiteral("bigg") ||
            name == QStringLiteral("Bigg") || name == QStringLiteral("bigl") || name == QStringLiteral("bigr") ||
            name == QStringLiteral("Bigl") || name == QStringLiteral("Bigr")) {
            return text(readDelimiter(), false, Cls::Ord, scale);
        }
        if (name == QStringLiteral("text") || name == QStringLiteral("textrm") || name == QStringLiteral("mbox") ||
            name == QStringLiteral("textnormal")) {
            return plainText(readRawGroup(), scale);
        }
        if (name == QStringLiteral("textbf")) return plainText(readRawGroup(), scale, true);
        if (name == QStringLiteral("mathrm") || name == QStringLiteral("operatorname") ||
            name == QStringLiteral("mathbf") || name == QStringLiteral("boldsymbol")) {
            const QString raw = readRawGroup();
            QFont font = scaledFont(ctx_.mathFont, em);
            font.setBold(name == QStringLiteral("mathbf") || name == QStringLiteral("boldsymbol"));
            return std::make_unique<TextBox>(raw, font, false,
                                             name == QStringLiteral("operatorname") ? Cls::Op : Cls::Ord);
        }
        if (name == QStringLiteral("overline") || name == QStringLiteral("bar")) {
            return std::make_unique<OverlineBox>(parseArg(scale, false), ctx_, scale);
        }
        if (name == QStringLiteral("underline") || name == QStringLiteral("boxed") ||
            name == QStringLiteral("mathit") || name == QStringLiteral("displaystyle") ||
            name == QStringLiteral("textstyle")) {
            if (name == QStringLiteral("displaystyle") || name == QStringLiteral("textstyle"))
                return std::make_unique<SpaceBox>(0);
            return parseArg(scale, display);
        }
        if (name == QStringLiteral("sum") || name == QStringLiteral("prod") || name == QStringLiteral("int")) {
            const QString glyph = name == QStringLiteral("sum") ? QStringLiteral("∑")
                                : name == QStringLiteral("prod") ? QStringLiteral("∏") : QStringLiteral("∫");
            return text(glyph, false, Cls::Op, scale * (display ? 1.35 : 1.1));
        }
        if (isFunctionName(name)) return text(name, false, Cls::Op, scale);
        if (const auto it = symbols().constFind(name); it != symbols().constEnd()) {
            const bool greekLower = it->text.size() == 1 && it->text.at(0).unicode() >= 0x03B1 &&
                                    it->text.at(0).unicode() <= 0x03C9;
            return text(it->text, greekLower, it->cls, scale);
        }
        // 未知命令的参数也按源码保留，不能再当普通分组解析并吞掉花括号。
        // 支持连续的可选/必选参数及嵌套、转义括号；尾随正文仍正常排版。
        while (!atEnd()) {
            int next = pos_;
            while (next < src_.size() && src_.at(next).isSpace()) ++next;
            if (next >= src_.size() ||
                (src_.at(next) != QLatin1Char('{') && src_.at(next) != QLatin1Char('['))) break;
            QString closers;
            pos_ = next;
            do {
                const QChar c = src_.at(pos_++);
                if (c == QLatin1Char('\\') && !atEnd()) { ++pos_; continue; }
                if (c == QLatin1Char('{')) closers += QLatin1Char('}');
                else if (c == QLatin1Char('[')) closers += QLatin1Char(']');
                else if (!closers.isEmpty() && c == closers.at(closers.size() - 1)) closers.chop(1);
            } while (!atEnd() && !closers.isEmpty());
        }
        return text(src_.mid(commandStart, pos_ - commandStart), false, Cls::Ord, scale, false);
    }

    BoxPtr parseAtom(qreal scale, bool display) {
        skipSpaces();
        if (atEnd()) return nullptr;
        const QChar c = peek();
        if (c == QLatin1Char('{')) {
            ++pos_;
            BoxPtr group = parseRow(scale, display, Stop::Brace);
            if (peek() == QLatin1Char('}')) ++pos_;
            return group;
        }
        if (c == QLatin1Char('}')) return nullptr;
        if (c == QLatin1Char('\\')) return parseCommand(scale, display);
        if (c.isDigit() || (c == QLatin1Char('.') && peek(1).isDigit())) {
            const int start = pos_;
            while (!atEnd() && (peek().isDigit() || (peek() == QLatin1Char('.') && peek(1).isDigit()))) ++pos_;
            return text(src_.mid(start, pos_ - start), false, Cls::Ord, scale);
        }
        if (c.unicode() < 128 && c.isLetter()) {
            ++pos_;
            return text(QString(c), true, Cls::Ord, scale);
        }
        ++pos_;
        switch (c.unicode()) {
        case '+': return text(QStringLiteral("+"), false, Cls::Bin, scale);
        case '-': return text(QStringLiteral("−"), false, Cls::Bin, scale);
        case '*': return text(QStringLiteral("×"), false, Cls::Bin, scale);
        case '=': case '<': case '>': case ':':
            return text(QString(c), false, Cls::Rel, scale);
        case ',': case ';': return text(QString(c), false, Cls::Punct, scale);
        case '(': case '[': return text(QString(c), false, Cls::Open, scale);
        case ')': case ']': return text(QString(c), false, Cls::Close, scale);
        case '\'': return text(QStringLiteral("′"), false, Cls::Ord, scale);
        case '&': case '~': return std::make_unique<SpaceBox>(ctx_.px * scale * 0.3);
        default: break;
        }
        static const QString binaries = QStringLiteral("×÷·±−∓");
        static const QString relations = QStringLiteral("≤≥≈≠≡∼→⇒∶");
        if (binaries.contains(c)) return text(QString(c), false, Cls::Bin, scale);
        if (relations.contains(c)) return text(QString(c), false, Cls::Rel, scale);
        if (isCjkOrText(c)) {
            // 公式里的中文按正文字体整段绘制（模型常把“本期”“上期”直接写进公式）。
            const int start = pos_ - 1;
            while (!atEnd() && isCjkOrText(peek()) && !binaries.contains(peek()) && !relations.contains(peek()))
                ++pos_;
            return plainText(src_.mid(start, pos_ - start), scale);
        }
        if (c.isLetter() && c.unicode() < 0x2000) return text(QString(c), true, Cls::Ord, scale);
        return text(QString(c), false, Cls::Ord, scale);
    }

    QString src_;
    const Context& ctx_;
    int pos_ = 0;
    int depth_ = 0;
};

Context makeContext(const MathStyle& style) {
    Context ctx;
    ctx.uiFont = style.font;
    ctx.px = QFontInfo(style.font).pixelSize();
    if (ctx.px <= 0) ctx.px = 14;
    ctx.mathFont = style.font;
    ctx.mathFont.setFamilies({QStringLiteral("Times New Roman"), QStringLiteral("STIX Two Text"),
                              QStringLiteral("Cambria"), QStringLiteral("Times"),
                              style.font.family()});
    ctx.mathFont.setBold(false);
    ctx.mathFont.setItalic(false);
    // 正文 12px 左右时分数线约 1 逻辑像素，高分屏上至少保留一个物理像素。
    const qreal dpr = std::max<qreal>(1.0, style.devicePixelRatio);
    ctx.thickness = std::max(1.0 / dpr, std::round(ctx.px * 0.065 * dpr) / dpr);
    return ctx;
}

}  // namespace

RenderedMath renderMath(const QString& latex, const MathStyle& style, bool display) {
    const Context ctx = makeContext(style);
    Parser parser(latex, ctx);
    const BoxPtr box = parser.parse(display);

    qreal scale = 1.0;
    const qreal margin = 1.0;
    if (style.maxWidth > 0 && box->width + 2 * margin > style.maxWidth)
        scale = std::max(0.6, (style.maxWidth - 2 * margin) / box->width);
    const qreal ascent = box->ascent * scale;
    const qreal descent = box->descent * scale;

    // Qt 富文本对 vertical-align:middle 的图片：当图片高于文字行时，基线位于
    // 距图片顶部 height/2 + xHeight/4 处。据此补白，让公式基线与正文基线重合；
    // 并保证图片至少比文字行高，始终处在这一对齐规则下。
    const QFontMetricsF fm(style.font);
    const qreal quarterX = fm.xHeight() / 4;
    qreal top = 0, bottom = 0;
    const qreal skew = (ascent - descent) - 2 * quarterX;
    if (skew > 0) bottom = skew;
    else top = -skew;
    qreal height = top + ascent + descent + bottom;
    const qreal minHeight = fm.height() * 1.35;
    if (height < minHeight) {
        top += (minHeight - height) / 2;
        bottom += (minHeight - height) / 2;
        height = minHeight;
    }
    const qreal dpr = std::max<qreal>(1.0, style.devicePixelRatio);
    RenderedMath result;
    result.width = static_cast<int>(std::ceil(box->width * scale + 2 * margin));
    result.height = static_cast<int>(std::ceil(height));
    result.image = QImage(static_cast<int>(std::ceil(result.width * dpr)),
                          static_cast<int>(std::ceil(result.height * dpr)),
                          QImage::Format_ARGB32_Premultiplied);
    result.image.fill(Qt::transparent);
    result.image.setDevicePixelRatio(dpr);
    QPainter painter(&result.image);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    painter.setPen(style.color);
    painter.translate(margin, top + ascent);
    painter.scale(scale, scale);
    box->draw(painter, 0, 0);
    return result;
}

QString mathHtml(const QString& latex, const MathStyle& style, bool display) {
    if (!display) {
        // 行内模式先不画图，看有没有二维结构。
        const Context ctx = makeContext(style);
        Parser parser(latex, ctx);
        const BoxPtr box = parser.parse(false);
        if (!box->needsImage()) return box->html();

        // 有分数、根号等二维结构时，以 0.85× 字号出图，避免行内行高膨胀。
        MathStyle compact = style;
        const int fullPx = ctx.px > 0 ? qRound(ctx.px) : QFontInfo(style.font).pixelSize();
        const int compactPx = fullPx > 0 ? std::max(7, qRound(fullPx * 0.85))
                                         : std::max(7, qRound(QFontMetrics(style.font).height() * 0.78));
        compact.font.setPixelSize(compactPx);
        const RenderedMath math = renderMath(latex, compact, false);
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        math.image.save(&buffer, "PNG");
        return QStringLiteral("<img src=\"data:image/png;base64,%1\" width=\"%2\" height=\"%3\" "
                              "style=\"vertical-align:middle\">")
            .arg(QString::fromLatin1(png.toBase64()))
            .arg(math.width)
            .arg(math.height);
    }
    const RenderedMath math = renderMath(latex, style, display);
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    math.image.save(&buffer, "PNG");
    return QStringLiteral("<img src=\"data:image/png;base64,%1\" width=\"%2\" height=\"%3\" "
                          "style=\"vertical-align:middle\">")
        .arg(QString::fromLatin1(png.toBase64()))
        .arg(math.width)
        .arg(math.height);
}

}  // namespace quizpane::ui
