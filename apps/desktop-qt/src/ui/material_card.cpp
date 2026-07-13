#include "material_card.hpp"
#include "line_icons.hpp"

#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QHBoxLayout>

namespace quizpane::ui {

MaterialCard::MaterialCard(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("materialCard"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(6);

    auto* header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    titleLabel_ = new QLabel;
    titleLabel_->setObjectName(QStringLiteral("materialCardTitle"));
    titleLabel_->setWordWrap(true);
    toggleButton_ = new QPushButton;
    toggleButton_->setObjectName(QStringLiteral("materialCardToggle"));
    toggleButton_->setFlat(true);
    toggleButton_->setFixedSize(22, 22);
    toggleButton_->setCursor(Qt::PointingHandCursor);
    header->addWidget(titleLabel_, 1);
    header->addWidget(toggleButton_);
    layout->addLayout(header);

    bodyScroll_ = new QScrollArea;
    bodyScroll_->setObjectName(QStringLiteral("materialCardBodyScroll"));
    bodyScroll_->setWidgetResizable(true);
    bodyScroll_->setFrameShape(QFrame::NoFrame);
    bodyScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    bodyScroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    // 材料正文可能有几百字，卡片本身不能无限撑高答题区，因此这里限制一个
    // 独立于外层 questionScroll_ 的最大高度，内部单独滚动。
    bodyScroll_->setMaximumHeight(160);
    bodyLabel_ = new QLabel;
    bodyLabel_->setObjectName(QStringLiteral("materialCardBody"));
    bodyLabel_->setWordWrap(true);
    bodyLabel_->setTextFormat(Qt::RichText);
    bodyLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bodyLabel_->setContentsMargins(0, 0, 4, 0);
    bodyScroll_->setWidget(bodyLabel_);
    layout->addWidget(bodyScroll_);

    connect(toggleButton_, &QPushButton::clicked, this, &MaterialCard::toggleCollapsed);
    setVisible(false);
    applyCollapsedState();
}

void MaterialCard::showMaterial(const QString& materialId, const QString& title,
                                const QString& contentHtml) {
    if (!materialId.isEmpty() && materialId == materialId_) {
        // 同一材料内切子题：控件和折叠状态原样保留，避免重复解析长文本 HTML。
        setVisible(true);
        return;
    }
    materialId_ = materialId;
    titleLabel_->setText(title.isEmpty() ? QStringLiteral("共享材料") : title);
    bodyLabel_->setText(contentHtml);
    // 首次进入某个材料组默认展开；只有材料切换时才重置折叠状态，用户在
    // 组内切题时手动折叠的选择应当保留。
    collapsed_ = false;
    applyCollapsedState();
    setVisible(true);
}

void MaterialCard::hideMaterial() {
    materialId_.clear();
    setVisible(false);
}

void MaterialCard::toggleCollapsed() {
    collapsed_ = !collapsed_;
    applyCollapsedState();
}

void MaterialCard::applyCollapsedState() {
    bodyScroll_->setVisible(!collapsed_);
    toggleButton_->setIcon(makeLineIcon(collapsed_ ? LineIcon::ChevronDown : LineIcon::ChevronUp));
    toggleButton_->setAccessibleName(collapsed_ ? QStringLiteral("展开材料") : QStringLiteral("收起材料"));
    toggleButton_->setToolTip(toggleButton_->accessibleName());
}

}  // namespace quizpane::ui
