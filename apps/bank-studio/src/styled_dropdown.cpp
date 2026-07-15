#include "styled_dropdown.hpp"

#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMouseEvent>
#include <QVBoxLayout>

namespace quizpane::studio {

StyledDropdown::StyledDropdown(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("styledDropdown"));
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    currentLabel_ = new QLabel(this);
    currentLabel_->setObjectName(QStringLiteral("styledDropdownLabel"));
    arrow_ = new QLabel(QStringLiteral("▾"), this);
    arrow_->setObjectName(QStringLiteral("styledDropdownArrow"));
    layout->addWidget(currentLabel_, 1);
    layout->addWidget(arrow_, 0);
}

void StyledDropdown::addItem(const QString& text, const QVariant& data) {
    addItem(QIcon(), text, data);
}

void StyledDropdown::addItem(const QIcon& icon, const QString& text, const QVariant& data) {
    entries_.append({icon, text, data});
    if (currentIndex_ < 0) selectRow(0);
}

void StyledDropdown::addItems(const QStringList& texts) {
    for (const QString& text : texts) addItem(text);
}

void StyledDropdown::clear() {
    entries_.clear();
    currentIndex_ = -1;
    refreshLabel();
}

void StyledDropdown::setCurrentIndex(int index) {
    if (index < 0 || index >= entries_.size() || index == currentIndex_) return;
    selectRow(index);
}

QVariant StyledDropdown::currentData() const {
    if (currentIndex_ < 0 || currentIndex_ >= entries_.size()) return {};
    return entries_.at(currentIndex_).data;
}

QString StyledDropdown::currentText() const {
    if (currentIndex_ < 0 || currentIndex_ >= entries_.size()) return {};
    return entries_.at(currentIndex_).text;
}

int StyledDropdown::findData(const QVariant& data) const {
    for (int i = 0; i < entries_.size(); ++i) {
        if (entries_.at(i).data == data) return i;
    }
    return -1;
}

void StyledDropdown::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) showPopup();
    QWidget::mousePressEvent(event);
}

void StyledDropdown::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
    case Qt::Key_Down:
    case Qt::Key_Up:
    case Qt::Key_Space:
    case Qt::Key_Return:
    case Qt::Key_Enter:
        showPopup();
        return;
    default:
        QWidget::keyPressEvent(event);
    }
}

void StyledDropdown::ensurePopup() {
    if (popup_) return;
    popup_ = new QFrame(this, Qt::Popup);
    popup_->setObjectName(QStringLiteral("styledDropdownPopup"));
    auto* popupLayout = new QVBoxLayout(popup_);
    popupLayout->setContentsMargins(4, 4, 4, 4);
    popupLayout->setSpacing(0);
    popupList_ = new QListWidget(popup_);
    popupList_->setFocusPolicy(Qt::StrongFocus);
    popupLayout->addWidget(popupList_);
    connect(popupList_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        selectRow(popupList_->row(item));
        popup_->hide();
    });
    connect(popupList_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        selectRow(popupList_->row(item));
        popup_->hide();
    });
}

void StyledDropdown::showPopup() {
    if (entries_.isEmpty()) return;
    ensurePopup();
    popupList_->clear();
    for (const Entry& entry : entries_) {
        auto* item = new QListWidgetItem(entry.icon, entry.text, popupList_);
        Q_UNUSED(item);
    }
    if (currentIndex_ >= 0) popupList_->setCurrentRow(currentIndex_);
    const int rowCount = qMin(entries_.size(), 8);
    const int rowHeight = popupList_->sizeHintForRow(0) > 0 ? popupList_->sizeHintForRow(0) : 28;
    popup_->setFixedWidth(qMax(width(), 160));
    popupList_->setFixedHeight(rowHeight * rowCount + 8);
    popup_->move(mapToGlobal(QPoint(0, height())));
    popup_->show();
    popupList_->setFocus();
}

void StyledDropdown::refreshLabel() {
    if (currentIndex_ < 0 || currentIndex_ >= entries_.size()) {
        currentLabel_->clear();
        currentLabel_->setPixmap(QPixmap());
        return;
    }
    const Entry& entry = entries_.at(currentIndex_);
    currentLabel_->setText(entry.text);
    if (!entry.icon.isNull()) {
        currentLabel_->setPixmap(entry.icon.pixmap(18, 18));
        currentLabel_->setText(QString());
        // 图标与文字目前互斥显示在同一个 QLabel 里；三处调用点中只有厂商下拉
        // 会传入图标，且该场景对齐即可满足需求，不需要额外的图文混排控件。
    }
}

void StyledDropdown::selectRow(int row) {
    if (row < 0 || row >= entries_.size()) return;
    const bool changed = row != currentIndex_;
    currentIndex_ = row;
    refreshLabel();
    if (changed) emit currentIndexChanged(currentIndex_);
}

}  // namespace quizpane::studio
