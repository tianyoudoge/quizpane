#pragma once

#include "math_formula.hpp"

#include <QColor>
#include <QLabel>
#include <QString>

namespace quizpane::ui {

struct AiResponseStyle {
    MathStyle math;
    QColor accent;     // 【答案确认】等分节标题
    QColor tipAccent;  // 【速算技巧】标题
};

// 对题干 HTML 的文字节点做公式转换（保留图片路径和标签属性）。
QString formatQuestionHtml(const QString& html);

// 把模型返回的 Markdown + LaTeX（$…$、$$…$$、\(…\)、\[…\]，以及未加定界符的
// \frac 等裸命令）转换为 QLabel 可显示的富文本；公式由 math_formula 渲染。
QString formatAiResponse(const QString& raw, const AiResponseStyle& style);

// 显示 AI 解析的标签。公式图片按当前字体、文字颜色和屏幕缩放绘制，
// 因此在切换主题、字号或屏幕时用保存的原文重新排版。
class AiResponseLabel final : public QLabel {
public:
    explicit AiResponseLabel(QWidget* parent = nullptr);
    void setResponse(const QString& raw);
    void setMessage(const QString& html);
    QString response() const { return raw_; }

protected:
    void changeEvent(QEvent* event) override;

private:
    void render();

    QString raw_;
    bool rendering_ = false;
};

}  // namespace quizpane::ui
