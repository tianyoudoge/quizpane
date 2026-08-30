#include "source_row_widget.hpp"
#include "source_validation.hpp"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QComboBox>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
#include <QPushButton>
#include <QUrl>

namespace quizpane::studio {
namespace {

constexpr int kNameLabelWidth = 190;

QLabel* elidedLabel(const QString& text, int width) {
    auto* label = new QLabel;
    label->setFixedWidth(width);
    label->setText(label->fontMetrics().elidedText(text, Qt::ElideMiddle, width));
    return label;
}

}  // namespace

SourceRowWidget::SourceRowWidget(const QString& questionPath, QWidget* parent)
    : QFrame(parent), questionPath_(questionPath) {
    setObjectName(QStringLiteral("sourceRow"));
    setAcceptDrops(true);
    setMinimumHeight(64);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(14, 10, 14, 10);
    layout->setSpacing(10);

    auto* icon = new QLabel;
    icon->setPixmap(QFileIconProvider().icon(QFileInfo(questionPath)).pixmap(22, 22));
    icon->setToolTip(questionPath);
    layout->addWidget(icon, 0);
    auto* name = elidedLabel(QFileInfo(questionPath).fileName(), kNameLabelWidth);
    name->setObjectName(QStringLiteral("sourceRowTitle"));
    name->setToolTip(questionPath);
    layout->addWidget(name, 0);
    layout->addStretch(1);

    answerLocation_ = new QComboBox;
    answerLocation_->setObjectName(QStringLiteral("answerLocation"));
    answerLocation_->addItem(QStringLiteral("自动检测"),
                             static_cast<int>(AnswerPolicyHint::Auto));
    answerLocation_->addItem(QStringLiteral("含答案"),
                             static_cast<int>(AnswerPolicyHint::Included));
    answerLocation_->addItem(QStringLiteral("无答案"),
                             static_cast<int>(AnswerPolicyHint::None));
    answerLocation_->setToolTip(QStringLiteral(
        "默认由程序识别是否包含答案；答案在另一份文档时，请使用右侧“添加单独答案”。"));
    connect(answerLocation_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) { emit answerPolicyChanged(answerPolicy()); });
    layout->addWidget(answerLocation_, 0);

    addAnswerButton_ = new QPushButton(QStringLiteral("添加单独答案"));
    addAnswerButton_->setObjectName(QStringLiteral("textButton"));
    addAnswerButton_->setToolTip(QStringLiteral(
        "如果答案在另一个文档里，可点击添加或直接把文档拖到这一行；\n"
        "原文档已包含答案则无需添加。"));
    connect(addAnswerButton_, &QPushButton::clicked, this, &SourceRowWidget::answerRequested);
    layout->addWidget(addAnswerButton_, 0);

    pairedAnswerLabel_ = new QLabel;
    pairedAnswerLabel_->setObjectName(QStringLiteral("pairedAnswerBadge"));
    pairedAnswerLabel_->setFixedWidth(kNameLabelWidth);
    pairedAnswerLabel_->setVisible(false);
    layout->addWidget(pairedAnswerLabel_, 0);

    clearAnswerButton_ = new QPushButton(QStringLiteral("取消配对"));
    clearAnswerButton_->setObjectName(QStringLiteral("textButton"));
    clearAnswerButton_->setVisible(false);
    connect(clearAnswerButton_, &QPushButton::clicked, this, &SourceRowWidget::answerCleared);
    layout->addWidget(clearAnswerButton_, 0);

    auto* removeButton = new QPushButton(QStringLiteral("移除"));
    removeButton->setObjectName(QStringLiteral("textButton"));
    connect(removeButton, &QPushButton::clicked, this, &SourceRowWidget::removeRequested);
    layout->addWidget(removeButton, 0);
}

AnswerPolicyHint SourceRowWidget::answerPolicy() const {
    return static_cast<AnswerPolicyHint>(answerLocation_->currentData().toInt());
}

void SourceRowWidget::setAnswerPolicy(AnswerPolicyHint policy) {
    const int index = answerLocation_->findData(static_cast<int>(policy));
    if (index >= 0)
        answerLocation_->setCurrentIndex(index);
}

void SourceRowWidget::setPairedAnswer(const QString& answerPath) {
    pairedAnswerLabel_->setText(
        pairedAnswerLabel_->fontMetrics().elidedText(
            QFileInfo(answerPath).fileName(), Qt::ElideMiddle, kNameLabelWidth));
    pairedAnswerLabel_->setToolTip(answerPath);
    addAnswerButton_->setVisible(false);
    pairedAnswerLabel_->setVisible(true);
    clearAnswerButton_->setVisible(true);
    setAnswerPolicy(AnswerPolicyHint::Included);
}

void SourceRowWidget::clearPairedAnswer() {
    pairedAnswerLabel_->setVisible(false);
    clearAnswerButton_->setVisible(false);
    addAnswerButton_->setVisible(true);
    setAnswerPolicy(AnswerPolicyHint::Auto);
}

void SourceRowWidget::dragEnterEvent(QDragEnterEvent* event) {
    for (const QUrl& url : event->mimeData()->urls()) {
        if (!url.isLocalFile() || !acceptedSource(url.toLocalFile())) continue;
        event->acceptProposedAction();
        return;
    }
}

void SourceRowWidget::dropEvent(QDropEvent* event) {
    for (const QUrl& url : event->mimeData()->urls()) {
        if (!url.isLocalFile() || !acceptedSource(url.toLocalFile())) continue;
        emit answerDropped(QFileInfo(url.toLocalFile()).absoluteFilePath());
        event->acceptProposedAction();
        return;
    }
}

}  // namespace quizpane::studio
