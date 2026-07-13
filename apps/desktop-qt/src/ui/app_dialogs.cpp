#include "app_dialogs.hpp"

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace quizpane::ui {

std::optional<QKeySequence> askBossKey(QWidget* parent,
                                       const QKeySequence& current) {
    // QDialog::exec() 会开启一个局部事件循环，行为类似同步等待模态框结果；
    // 但 Qt 主事件循环仍在处理重绘和按钮点击，所以界面不会“卡死”。
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("老板键设置"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* description = new QLabel(
        QStringLiteral("按下组合键可立即隐藏或显示小窗。请使用修饰键加字母或数字。"));
    description->setWordWrap(true);
    auto* editor = new QKeySequenceEdit(current, &dialog);
    editor->setClearButtonEnabled(true);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));

    // signal/slot 可以类比 jQuery 的 .on("click", handler)，但连接在编译期
    // 受 C++ 类型检查保护。父子对象关系还会自动负责内存释放。
    QObject::connect(buttons, &QDialogButtonBox::accepted,
                     &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected,
                     &dialog, &QDialog::reject);
    layout->addWidget(description);
    layout->addWidget(editor);
    layout->addWidget(buttons);
    dialog.setMinimumWidth(340);

    if (dialog.exec() != QDialog::Accepted) return std::nullopt;
    return editor->keySequence();
}

void showAbout(QWidget* parent) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("关于小窗刷题"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->setAlignment(Qt::AlignHCenter);
    auto* icon = new QLabel;
    icon->setPixmap(QApplication::windowIcon().pixmap(88, 88));
    icon->setAlignment(Qt::AlignCenter);
    auto* info = new QLabel(QStringLiteral(
        "<div align='center'><h2>小窗刷题</h2>"
        "<p>版本 %1</p>"
        "<p>作者：xutianyou<br>"
        "联系：xutianyoubupt@foxmail.com</p>"
        "<p>Copyright © 2026 xutianyou<br>"
        "个人非商业使用免费<br>"
        "PolyForm Noncommercial 1.0.0<br>"
        "商业使用请联系作者授权</p></div>")
        .arg(QApplication::applicationVersion()));
    info->setTextFormat(Qt::RichText);
    info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    close->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    QObject::connect(close, &QDialogButtonBox::rejected,
                     &dialog, &QDialog::reject);
    layout->addWidget(icon);
    layout->addWidget(info);
    layout->addWidget(close);
    dialog.setFixedWidth(340);
    dialog.exec();
}

}  // namespace quizpane::ui
