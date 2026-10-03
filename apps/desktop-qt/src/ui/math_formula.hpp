#pragma once

#include <QColor>
#include <QFont>
#include <QImage>
#include <QString>

namespace quizpane::ui {

struct MathStyle {
    QFont font;                  // 正文字体；公式字号跟随它的像素大小
    QColor color = Qt::black;
    qreal devicePixelRatio = 1.0;
    int maxWidth = 0;            // 逻辑像素；0 表示不限制。超宽公式整体缩小（最多到 60%）
};

struct RenderedMath {
    QImage image;                // 已设置 devicePixelRatio
    int width = 0;               // 逻辑像素
    int height = 0;
};

// 渲染 LaTeX 数学子集：分数、根号、上下标、\left/\right 与自动拉伸括号、常用符号。
// 图片上下补白，使 Qt 富文本 vertical-align:middle 时公式基线与正文基线重合。
// 不认识的命令按源码原样绘制，便于用户核对。
RenderedMath renderMath(const QString& latex, const MathStyle& style, bool display);

// 公式的富文本片段：含分数、根号等二维结构或独立成行时为内嵌 PNG 的 <img>，
// 否则为斜体字母 + <sup>/<sub> 的普通 HTML，避免简单变量撑高行距。
QString mathHtml(const QString& latex, const MathStyle& style, bool display);

}  // namespace quizpane::ui
