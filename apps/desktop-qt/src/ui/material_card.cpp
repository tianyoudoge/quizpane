#include "material_card.hpp"
#include "line_icons.hpp"

#include <QDialog>
#include <QDialogButtonBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QScreen>
#include <QSizePolicy>
#include <QUrl>
#include <QVBoxLayout>

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
    bodyContent_ = new QWidget;
    bodyContent_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    bodyLayout_ = new QVBoxLayout(bodyContent_);
    bodyLayout_->setContentsMargins(0, 0, 4, 0);
    bodyLayout_->setSpacing(8);
    bodyLabel_ = new QLabel;
    bodyLabel_->setObjectName(QStringLiteral("materialCardBody"));
    bodyLabel_->setWordWrap(true);
    bodyLabel_->setTextFormat(Qt::RichText);
    bodyLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // QLabel 的富文本只有明确可用宽度才会按卡片宽度重新排版；Ignored 防止
    // 长中文行的 sizeHint 撑开横向滚动区，导致右侧文字不可见。
    bodyLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    bodyLayout_->addWidget(bodyLabel_);
    bodyScroll_->setWidget(bodyContent_);
    layout->addWidget(bodyScroll_);

    connect(toggleButton_, &QPushButton::clicked, this, &MaterialCard::toggleCollapsed);
    setVisible(false);
    applyCollapsedState();
}

void MaterialCard::showMaterial(const QString& materialId, const QString& title,
                                const QString& contentHtml, const QJsonArray& imageUrls) {
    if (!materialId.isEmpty() && materialId == materialId_) {
        // 同一材料内切子题：控件和折叠状态原样保留，避免重复解析长文本 HTML。
        setVisible(true);
        return;
    }
    materialId_ = materialId;
    titleLabel_->setText(title.isEmpty() ? QStringLiteral("共享材料") : title);
    bodyLabel_->setText(contentHtml);
    rebuildImagePreviews(imageUrls);
    // 首次进入某个材料组默认展开；只有材料切换时才重置折叠状态，用户在
    // 组内切题时手动折叠的选择应当保留。
    collapsed_ = false;
    applyCollapsedState();
    setVisible(true);
    updateBodyWidth();
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

void MaterialCard::rebuildImagePreviews(const QJsonArray& imageUrls) {
    while (bodyLayout_->count() > 1) {
        QLayoutItem* item = bodyLayout_->takeAt(1);
        delete item->widget();
        delete item;
    }
    imagePreviews_.clear();
    int ordinal = 0;
    for (const QJsonValue& value : imageUrls) {
        const QString imageUrl = value.toString();
        const QPixmap pixmap(QUrl(imageUrl).toLocalFile());
        if (imageUrl.isEmpty() || pixmap.isNull())
            continue;
        auto* preview = new QPushButton;
        preview->setObjectName(QStringLiteral("materialImagePreview"));
        preview->setFlat(true);
        preview->setCursor(Qt::PointingHandCursor);
        preview->setAccessibleName(QStringLiteral("查看材料原图 %1").arg(++ordinal));
        preview->setToolTip(QStringLiteral("点击按原始尺寸查看"));
        preview->setIcon(QIcon(pixmap));
        connect(preview, &QPushButton::clicked, this,
                [this, imageUrl] { openImagePreview(imageUrl); });
        bodyLayout_->addWidget(preview, 0, Qt::AlignLeft);
        imagePreviews_.append({preview, pixmap});
    }
}

void MaterialCard::updateBodyWidth() {
    if (!bodyScroll_ || !bodyContent_ || !bodyLabel_)
        return;
    const int availableWidth = qMax(1, bodyScroll_->viewport()->width() - 4);
    bodyContent_->setFixedWidth(availableWidth);
    bodyLabel_->setFixedWidth(availableWidth);
    for (const ImagePreview& image : imagePreviews_) {
        if (!image.button || image.pixmap.isNull())
            continue;
        const QSize previewSize = image.pixmap.size().scaled(
            QSize(availableWidth, 260), Qt::KeepAspectRatio);
        image.button->setIconSize(previewSize);
        image.button->setFixedSize(previewSize + QSize(2, 2));
    }
}

void MaterialCard::openImagePreview(const QString& imageUrl) const {
    const QPixmap pixmap(QUrl(imageUrl).toLocalFile());
    if (pixmap.isNull())
        return;
    QDialog dialog(const_cast<MaterialCard*>(this));
    dialog.setObjectName(QStringLiteral("materialImagePreviewDialog"));
    dialog.setWindowTitle(QStringLiteral("材料原图 · 原始尺寸"));
    dialog.setModal(true);
    auto* layout = new QVBoxLayout(&dialog);
    auto* hint = new QLabel(QStringLiteral("原始尺寸：%1 × %2；可滚动查看全部内容")
        .arg(pixmap.width()).arg(pixmap.height()));
    hint->setObjectName(QStringLiteral("materialImagePreviewHint"));
    layout->addWidget(hint);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(false);
    scroll->setAlignment(Qt::AlignCenter);
    auto* image = new QLabel;
    image->setPixmap(pixmap);  // 不缩放，保持原图像素尺寸。
    image->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    scroll->setWidget(image);
    layout->addWidget(scroll, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    const QRect available = (window() && window()->screen() ? window()->screen()
        : QGuiApplication::primaryScreen())->availableGeometry();
    dialog.resize(qMin(pixmap.width() + 48, available.width() * 9 / 10),
                  qMin(pixmap.height() + 108, available.height() * 9 / 10));
    dialog.exec();
}

void MaterialCard::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateBodyWidth();
}

}  // namespace quizpane::ui
