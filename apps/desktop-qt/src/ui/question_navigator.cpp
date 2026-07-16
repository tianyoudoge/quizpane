#include "question_navigator.hpp"

#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QVBoxLayout>

namespace quizpane::ui {

namespace {
constexpr int kColumns = 6;
}

QuestionNavigator::QuestionNavigator(QWidget* parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint) {
    setObjectName(QStringLiteral("questionNavigatorPopup"));
    setFixedWidth(300);
    setMaximumHeight(320);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 11, 12, 12);
    root->setSpacing(8);
    auto* title = new QLabel(QStringLiteral("选题清单"));
    title->setObjectName(QStringLiteral("questionNavigatorTitle"));
    summaryLabel_ = new QLabel;
    summaryLabel_->setObjectName(QStringLiteral("questionNavigatorSummary"));
    root->addWidget(title);
    root->addWidget(summaryLabel_);

    auto* scroll = new QScrollArea;
    scroll->setObjectName(QStringLiteral("questionNavigatorScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget;
    grid_ = new QGridLayout(content);
    grid_->setContentsMargins(0, 0, 4, 0);
    grid_->setHorizontalSpacing(6);
    grid_->setVerticalSpacing(6);
    scroll->setWidget(content);
    root->addWidget(scroll, 1);
}

void QuestionNavigator::setState(int questionCount, const QSet<int>& answered,
                                 int currentIndex) {
    questionCount = qMax(0, questionCount);
    if (buttons_.size() != questionCount) rebuild(questionCount);
    summaryLabel_->setText(QStringLiteral("已作答 %1 / %2 题").arg(answered.size()).arg(questionCount));
    for (int index = 0; index < buttons_.size(); ++index)
        refreshButtonStyle(buttons_.at(index), answered.contains(index), index == currentIndex);
}

QPushButton* QuestionNavigator::questionButton(int index) const {
    return index >= 0 && index < buttons_.size() ? buttons_.at(index) : nullptr;
}

void QuestionNavigator::rebuild(int questionCount) {
    while (QLayoutItem* item = grid_->takeAt(0)) {
        if (QWidget* widget = item->widget()) widget->deleteLater();
        delete item;
    }
    buttons_.clear();
    buttons_.reserve(questionCount);
    for (int index = 0; index < questionCount; ++index) {
        auto* button = new QPushButton(QString::number(index + 1));
        button->setObjectName(QStringLiteral("questionNumberButton"));
        button->setAccessibleName(QStringLiteral("跳转到第 %1 题").arg(index + 1));
        button->setToolTip(QStringLiteral("第 %1 题").arg(index + 1));
        button->setFixedSize(38, 34);
        grid_->addWidget(button, index / kColumns, index % kColumns);
        connect(button, &QPushButton::clicked, this, [this, index] {
            emit questionSelected(index);
            hide();
        });
        buttons_.append(button);
    }
    if (questionCount > 0) grid_->setRowStretch((questionCount - 1) / kColumns + 1, 1);
}

void QuestionNavigator::refreshButtonStyle(QPushButton* button, bool answered,
                                           bool current) {
    if (!button) return;
    button->setProperty("answered", answered);
    button->setProperty("current", current);
    button->style()->unpolish(button);
    button->style()->polish(button);
}

}  // namespace quizpane::ui
