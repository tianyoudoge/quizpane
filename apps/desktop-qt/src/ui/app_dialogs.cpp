#include "app_dialogs.hpp"

#include "quizpane/feedback_report.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace quizpane::ui {

bool confirm(QWidget* parent, const QString& title, const QString& message,
             const QString& confirmText, const QString& cancelText) {
    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    auto* layout = new QVBoxLayout(&dialog);
    auto* label = new QLabel(message);
    label->setWordWrap(true);
    auto* buttons = new QDialogButtonBox;
    auto* cancel = buttons->addButton(cancelText, QDialogButtonBox::RejectRole);
    auto* accept = buttons->addButton(confirmText, QDialogButtonBox::AcceptRole);
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(accept, &QPushButton::clicked, &dialog, &QDialog::accept);
    layout->addWidget(label);
    layout->addWidget(buttons);
    dialog.setMinimumWidth(340);
    return dialog.exec() == QDialog::Accepted;
}

DraftDecision chooseDraft(QWidget* parent, const QList<DraftSnapshot>& drafts) {
    if (drafts.isEmpty()) return {};
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("恢复未完成练习"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* description = new QLabel(QStringLiteral(
        "选择要继续的练习。稍后处理会完整保留草稿；只有“删除草稿”会移除所选记录。"));
    description->setWordWrap(true);
    auto* list = new QListWidget;
    for (const DraftSnapshot& draft : drafts) {
        const QString time = draft.updatedAt.isValid()
            ? draft.updatedAt.toLocalTime().toString(QStringLiteral("MM-dd HH:mm"))
            : QStringLiteral("时间未知");
        list->addItem(QStringLiteral("%1 · %2 · 第 %3 题")
            .arg(draft.title.isEmpty() ? QStringLiteral("未命名练习") : draft.title,
                 time).arg(draft.currentQuestionIndex + 1));
    }
    list->setCurrentRow(0);
    auto* buttons = new QDialogButtonBox;
    auto* discard = buttons->addButton(QStringLiteral("删除草稿"), QDialogButtonBox::DestructiveRole);
    auto* later = buttons->addButton(QStringLiteral("稍后处理"), QDialogButtonBox::RejectRole);
    auto* restore = buttons->addButton(QStringLiteral("继续练习"), QDialogButtonBox::AcceptRole);
    DraftDecision decision;
    QObject::connect(restore, &QPushButton::clicked, &dialog, [&] {
        decision = {DraftChoice::Restore, list->currentRow()}; dialog.accept();
    });
    QObject::connect(later, &QPushButton::clicked, &dialog, [&] {
        decision = {DraftChoice::Later, list->currentRow()}; dialog.reject();
    });
    QObject::connect(discard, &QPushButton::clicked, &dialog, [&] {
        decision = {DraftChoice::Discard, list->currentRow()}; dialog.done(2);
    });
    layout->addWidget(description);
    layout->addWidget(list);
    layout->addWidget(buttons);
    dialog.resize(430, 280);
    dialog.exec();
    return decision;
}

std::optional<QKeySequence> askBossKey(QWidget* parent,
                                       const QKeySequence& current) {
    // QDialog::exec() 会开启一个局部事件循环，行为类似同步等待模态框结果；
    // 但 Qt 主事件循环仍在处理重绘和按钮点击，所以界面不会“卡死”。
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("老板键设置"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* description = new QLabel(
        QStringLiteral("按下组合键可立即隐藏或显示小窗。支持修饰键加字母、数字或 F1–F12。"));
    description->setWordWrap(true);
    auto* editor = new QKeySequenceEdit(current, &dialog);
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    editor->setClearButtonEnabled(true);
#endif
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

void showDonation(QWidget* parent) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("赞赏支持"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 22, 24, 18);
    layout->setSpacing(10);
    auto* title = new QLabel(QStringLiteral("<h2 align='center'>请作者喝杯咖啡</h2>"));
    auto* description = new QLabel(QStringLiteral(
        "一个人慢慢把小窗刷题做好并不容易。您的支持，是我继续下去的最大动力。"));
    description->setAlignment(Qt::AlignCenter);
    description->setWordWrap(true);
    auto* code = new QLabel;
    code->setAlignment(Qt::AlignCenter);
    auto* caption = new QLabel;
    caption->setAlignment(Qt::AlignCenter);
    caption->setStyleSheet(QStringLiteral("color: #7d8794; font-size: 12px;"));
    auto* paymentRow = new QHBoxLayout;
    auto* previous = new QPushButton(QStringLiteral("‹"));
    auto* wechat = new QPushButton(QStringLiteral("微信支付"));
    auto* alipay = new QPushButton(QStringLiteral("支付宝"));
    auto* next = new QPushButton(QStringLiteral("›"));
    wechat->setCheckable(true);
    alipay->setCheckable(true);
    paymentRow->addStretch();
    paymentRow->addWidget(previous);
    paymentRow->addWidget(wechat);
    paymentRow->addWidget(alipay);
    paymentRow->addWidget(next);
    paymentRow->addStretch();
    bool showingAlipay = false;
    const auto updatePayment = [&] {
        const QString resource = showingAlipay
            ? QStringLiteral(":/icons/alipay-payment.jpg")
            : QStringLiteral(":/icons/wechat-payment.jpg");
        code->setPixmap(QPixmap(resource).scaled(
            220, 220, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        caption->setText(showingAlipay ? QStringLiteral("支付宝扫码赞赏")
                                       : QStringLiteral("微信扫码赞赏"));
        wechat->setChecked(!showingAlipay);
        alipay->setChecked(showingAlipay);
    };
    QObject::connect(previous, &QPushButton::clicked, &dialog, [&] {
        showingAlipay = !showingAlipay; updatePayment();
    });
    QObject::connect(next, &QPushButton::clicked, &dialog, [&] {
        showingAlipay = !showingAlipay; updatePayment();
    });
    QObject::connect(wechat, &QPushButton::clicked, &dialog, [&] {
        showingAlipay = false; updatePayment();
    });
    QObject::connect(alipay, &QPushButton::clicked, &dialog, [&] {
        showingAlipay = true; updatePayment();
    });
    updatePayment();
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    close->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    QObject::connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(title);
    layout->addWidget(description);
    layout->addWidget(code);
    layout->addWidget(caption);
    layout->addLayout(paymentRow);
    layout->addWidget(close);
    dialog.setFixedWidth(360);
    dialog.exec();
}

bool showFeedback(QWidget* parent, const QString& endpoint) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("问题反馈"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 20, 24, 18);
    layout->setSpacing(10);
    auto* hint = new QLabel(QStringLiteral(
        "请尽量描述复现步骤和实际/预期表现。发送会附带运行环境信息；"
        "勾选的日志与崩溃信息已做脱敏（不含账号、题目与完整路径），"
        "将上传到 xutianyou.cc 供排查。"));
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: #7d8794; font-size: 12px;"));
    auto* editor = new QPlainTextEdit;
    editor->setPlaceholderText(QStringLiteral("出了什么问题？怎么触发的？"));
    editor->setFixedHeight(140);
    auto* logsCheck = new QCheckBox(QStringLiteral("附上最近的运行日志（已脱敏）"));
    logsCheck->setChecked(true);
    auto* crashCheck = new QCheckBox(QStringLiteral("附上崩溃信息（如有，最近 24 小时内）"));
    crashCheck->setChecked(true);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto* send = buttons->addButton(QStringLiteral("发送"), QDialogButtonBox::AcceptRole);
    send->setEnabled(false);
    QObject::connect(editor, &QPlainTextEdit::textChanged, [send](const QString& text) {
        send->setEnabled(!text.trimmed().isEmpty());
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(send, &QPushButton::clicked, [&dialog, endpoint, logsCheck, crashCheck,
                                                    editor] {
        const auto result =
            feedback::sendReport(editor->toPlainText(), logsCheck->isChecked(),
                                 crashCheck->isChecked(), endpoint);
        if (result.success) {
            dialog.accept();
            QMessageBox::information(&dialog, QStringLiteral("问题反馈"), result.message);
        } else {
            QMessageBox::warning(&dialog, QStringLiteral("问题反馈"), result.message);
        }
    });
    layout->addWidget(hint);
    layout->addWidget(editor);
    layout->addWidget(logsCheck);
    layout->addWidget(crashCheck);
    layout->addWidget(buttons);
    dialog.setMinimumWidth(440);
    return dialog.exec() == QDialog::Accepted;
}

}  // namespace quizpane::ui
