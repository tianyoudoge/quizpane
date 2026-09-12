#include "review/review_page_controller.hpp"

#include "quizpane/bank_validator.hpp"
#include "quizpane/diagnostic_logger.hpp"
#include "quizpane/studio/pdf_memory_budget.hpp"
#include "review/review_draft_bank.hpp"

#include <QBuffer>
#include <QButtonGroup>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSplitter>
#include <QStringList>
#include <QTextEdit>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>
#include <QtMath>

#include <algorithm>
#include <functional>
#include <limits>

namespace quizpane::studio {
namespace {

QLabel* mutedLabel(const QString& text) {
    auto* label = new QLabel(text);
    label->setObjectName(QStringLiteral("muted"));
    label->setWordWrap(true);
    return label;
}

QString reviewQuestionTitle(const QJsonObject& question) {
    const auto source = question.value("source").toObject();
    const int number = source.value("questionNumber").toInt();
    QString label = source.value("questionLabel").toString();
    if (label.isEmpty() || label == QString::number(number))
        label = number > 0 ? QStringLiteral("第 %1 题").arg(number) : question.value("id").toString();
    const QString section = source.value("sectionTitle").toString();
    if (!section.isEmpty()) label.prepend(section + QStringLiteral(" · "));
    return label;
}

QString materialPreviewHtml(const QString& text, const QJsonArray& underlines) {
    QList<QPair<int, int>> ranges;
    for (const QJsonValue& value : underlines) {
        const QJsonObject range = value.toObject();
        const int start = range.value(QStringLiteral("start")).toInt(-1);
        const int length = range.value(QStringLiteral("length")).toInt();
        if (start >= 0 && length > 0 && start + length <= text.size())
            ranges.append({start, length});
    }
    std::sort(ranges.begin(), ranges.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });
    QString html;
    int cursor = 0;
    for (const auto& range : ranges) {
        if (range.first < cursor) continue;
        html += text.mid(cursor, range.first - cursor).toHtmlEscaped();
        html += QStringLiteral("<span style=\"text-decoration:underline; text-decoration-thickness:1px;\">%1</span>")
            .arg(text.mid(range.first, range.second).toHtmlEscaped());
        cursor = range.first + range.second;
    }
    html += text.mid(cursor).toHtmlEscaped();
    const QRegularExpression blank(QStringLiteral("(?:〔填空〕|_{2,}|＿{2,})"));
    html.replace(blank, QStringLiteral("<span style=\"text-decoration:underline; letter-spacing:2px;\">&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;</span>"));
    html.replace(QStringLiteral("\n"), QStringLiteral("<br/>"));
    return html;
}

QString stemForReviewEditor(QString stem) {
    stem.replace(QStringLiteral("〔填空〕"), QStringLiteral("＿＿＿＿"));
    return stem;
}

QString stemFromReviewEditor(QString stem) {
    stem.replace(QRegularExpression(QStringLiteral("(?:_{2,}|＿{2,})")),
                 QStringLiteral("〔填空〕"));
    return stem;
}

void clearLayout(QLayout* layout) {
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* widget = item->widget())
            delete widget;
        if (QLayout* nested = item->layout()) {
            clearLayout(nested);
        }
        delete item;
    }
}

QRectF cropRectFromJson(const QJsonObject& value) {
    const QRectF crop(value.value(QStringLiteral("x")).toDouble(),
                      value.value(QStringLiteral("y")).toDouble(),
                      value.value(QStringLiteral("width")).toDouble(),
                      value.value(QStringLiteral("height")).toDouble());
    return crop.intersected(QRectF(0.0, 0.0, 1.0, 1.0));
}

QJsonObject cropRectToJson(const QRectF& crop) {
    return {{QStringLiteral("x"), crop.x()}, {QStringLiteral("y"), crop.y()},
            {QStringLiteral("width"), crop.width()}, {QStringLiteral("height"), crop.height()}};
}

QImage cropNormalizedImage(const QImage& page, const QRectF& normalizedCrop) {
    const QRect pixels(qFloor(normalizedCrop.x() * page.width()),
                       qFloor(normalizedCrop.y() * page.height()),
                       qMax(1, qCeil(normalizedCrop.width() * page.width())),
                       qMax(1, qCeil(normalizedCrop.height() * page.height())));
    return page.copy(pixels.intersected(page.rect()));
}

QRectF cropContextAround(const QRectF& crop) {
    const qreal horizontalPadding = qMax<qreal>(0.08, crop.width() * 0.85);
    const qreal verticalPadding = qMax<qreal>(0.10, crop.height() * 0.85);
    return QRectF(crop.x() - horizontalPadding, crop.y() - verticalPadding,
                  crop.width() + horizontalPadding * 2.0,
                  crop.height() + verticalPadding * 2.0)
        .intersected(QRectF(0.0, 0.0, 1.0, 1.0));
}

class CropCanvas final : public QWidget {
public:
    CropCanvas(const QImage& image, const QRectF& crop, QWidget* parent = nullptr)
        : QWidget(parent), image_(image), selection_(crop) {
        const QSize limited = image.size().scaled(QSize(980, 680), Qt::KeepAspectRatio);
        setFixedSize(limited.isEmpty() ? QSize(640, 420) : limited);
        setCursor(Qt::CrossCursor);
    }

    QRectF selection() const { return selection_; }
    void setSelection(const QRectF& crop) { selection_ = crop; update(); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(QStringLiteral("#dfe4ea")));
        painter.drawImage(rect(), image_);
        const QRect selectionRect(qRound(selection_.x() * width()),
                                  qRound(selection_.y() * height()),
                                  qRound(selection_.width() * width()),
                                  qRound(selection_.height() * height()));
        painter.setBrush(QColor(255, 77, 133, 30));
        painter.setPen(QPen(QColor(QStringLiteral("#ff4d85")), 2));
        painter.drawRect(selectionRect);
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton)
            return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        dragStart_ = normalized(event->position());
#else
        dragStart_ = normalized(event->localPos());
#endif
        selection_ = QRectF(dragStart_, QSizeF());
        update();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (!(event->buttons() & Qt::LeftButton))
            return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const QPointF position = event->position();
#else
        const QPointF position = event->localPos();
#endif
        selection_ = QRectF(dragStart_, normalized(position)).normalized()
            .intersected(QRectF(0.0, 0.0, 1.0, 1.0));
        update();
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton)
            mouseMoveEvent(event);
    }

private:
    QPointF normalized(const QPointF& position) const {
        return {qBound(0.0, position.x() / qMax(1, width()), 1.0),
                qBound(0.0, position.y() / qMax(1, height()), 1.0)};
    }

    QImage image_;
    QRectF selection_;
    QPointF dragStart_;
};

class CropDialog final : public QDialog {
public:
    CropDialog(const QImage& page, const QRectF& pageContext, const QRectF& automaticCrop,
               QWidget* parent = nullptr)
        : QDialog(parent), pageContext_(pageContext), automaticCrop_(automaticCrop) {
        setWindowTitle(QStringLiteral("从原卷重新裁切"));
        auto* layout = new QVBoxLayout(this);
        auto* description = mutedLabel(QStringLiteral(
            "这里仅显示当前定位附近的原卷区域，粉色框是当前裁切框。拖拽重新框选要保留的区域；"
            "不会上传原卷或图片。"));
        layout->addWidget(description);
        const QRect contextPixels(qFloor(pageContext.x() * page.width()),
                                  qFloor(pageContext.y() * page.height()),
                                  qCeil(pageContext.width() * page.width()),
                                  qCeil(pageContext.height() * page.height()));
        const QImage contextImage = page.copy(contextPixels.intersected(page.rect()));
        canvas_ = new CropCanvas(contextImage, toLocal(automaticCrop), this);
        layout->addWidget(canvas_, 0, Qt::AlignCenter);
        auto* buttons = new QDialogButtonBox;
        auto* reset = buttons->addButton(QStringLiteral("恢复当前定位"), QDialogButtonBox::ResetRole);
        auto* cancel = buttons->addButton(QStringLiteral("取消"), QDialogButtonBox::RejectRole);
        auto* useCrop = buttons->addButton(QStringLiteral("使用此裁切"), QDialogButtonBox::AcceptRole);
        QObject::connect(reset, &QPushButton::clicked, this,
                         [this] { canvas_->setSelection(toLocal(automaticCrop_)); });
        QObject::connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        QObject::connect(useCrop, &QPushButton::clicked, this, [this] {
            if (canvas_->selection().width() < 0.01 || canvas_->selection().height() < 0.01) {
                QMessageBox::warning(this, QStringLiteral("裁切范围过小"),
                    QStringLiteral("请拖拽出一个足够大的矩形区域。"));
                return;
            }
            QDialog::accept();
        });
        layout->addWidget(buttons);
    }

    QRectF selection() const { return toPage(canvas_->selection()); }
    void setSelection(const QRectF& crop) { canvas_->setSelection(toLocal(crop)); }

private:
    QRectF toLocal(const QRectF& crop) const {
        return {(crop.x() - pageContext_.x()) / pageContext_.width(),
                (crop.y() - pageContext_.y()) / pageContext_.height(),
                crop.width() / pageContext_.width(), crop.height() / pageContext_.height()};
    }
    QRectF toPage(const QRectF& crop) const {
        return {pageContext_.x() + crop.x() * pageContext_.width(),
                pageContext_.y() + crop.y() * pageContext_.height(),
                crop.width() * pageContext_.width(), crop.height() * pageContext_.height()};
    }
    CropCanvas* canvas_ = nullptr;
    QRectF pageContext_;
    QRectF automaticCrop_;
};

}  // namespace

void ReviewPageController::init(QWidget* parentForDialogs, const QStringList& sourcePaths,
                                std::function<void(int)> onUpdateNavigation) {
    parentWidget_ = parentForDialogs;
    sourcePaths_ = &sourcePaths;
    onUpdateNavigation_ = std::move(onUpdateNavigation);
}

QWidget* ReviewPageController::buildPage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(16);
    {
        auto* header = new QWidget;
        auto* headerLayout = new QVBoxLayout(header);
        headerLayout->setContentsMargins(0, 0, 0, 0);
        headerLayout->setSpacing(6);
        auto* eyebrow = new QLabel(QStringLiteral("第三步"));
        eyebrow->setObjectName(QStringLiteral("eyebrow"));
        auto* title = new QLabel(QStringLiteral("检查题目"));
        title->setObjectName(QStringLiteral("pageTitle"));
        headerLayout->addWidget(eyebrow);
        headerLayout->addWidget(title);
        headerLayout->addWidget(mutedLabel(QStringLiteral(
            "图片和扫描件已自动处理。只需处理标红的问题，也可以直接继续设置题库。")));
        layout->addWidget(header);
    }
    auto* filterBar = new QFrame;
    filterBar->setObjectName(QStringLiteral("reviewFilterBar"));
    auto* filters = new QHBoxLayout(filterBar);
    filters->setContentsMargins(0, 0, 0, 0);
    filters->setSpacing(0);
    reviewFilterGroup_ = new QButtonGroup(page);
    reviewFilterGroup_->setExclusive(true);
    allReviewButton_ = new QPushButton(QStringLiteral("需要处理  0"));
    allQuestionsButton_ = new QPushButton(QStringLiteral("全部题目  0"));
    missingAnswerButton_ = new QPushButton(QStringLiteral("缺少答案  0"));
    duplicateButton_ = new QPushButton(QStringLiteral("疑似重复  0"));
    allQuestionsButton_->setProperty("reviewFilter", QString());
    allReviewButton_->setProperty("reviewFilter", QStringLiteral("__any_review__"));
    missingAnswerButton_->setProperty("reviewFilter", QStringLiteral("__missing_answer__"));
    duplicateButton_->setProperty("reviewFilter", QStringLiteral("__duplicate__"));
    for (auto* button : {allReviewButton_, allQuestionsButton_, missingAnswerButton_, duplicateButton_}) {
        button->setObjectName(QStringLiteral("reviewFilterTab"));
        button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor);
        reviewFilterGroup_->addButton(button);
        filters->addWidget(button);
        QObject::connect(button, &QPushButton::clicked, page, [this, button] {
            activeReviewFilter_ = button->property("reviewFilter").toString();
            applyReviewFilter();
        });
    }
    filters->addStretch();
    layout->addWidget(filterBar);

    riskCategoryPanel_ = new QFrame;
    riskCategoryPanel_->setObjectName(QStringLiteral("panel"));
    riskCategoryLayout_ = new QVBoxLayout(riskCategoryPanel_);
    riskCategoryLayout_->setContentsMargins(16, 12, 16, 12);
    riskCategoryLayout_->setSpacing(8);
    riskCategoryPanel_->setVisible(false);
    layout->addWidget(riskCategoryPanel_);

    auto* reviewSplit = new QSplitter(Qt::Horizontal);
    reviewSplit->setObjectName(QStringLiteral("reviewSplit"));
    reviewSplit->setChildrenCollapsible(false);
    auto* navigator = new QWidget;
    auto* navigatorLayout = new QVBoxLayout(navigator);
    navigatorLayout->setContentsMargins(0, 0, 0, 0);
    navigatorLayout->setSpacing(8);
    navigatorLayout->addWidget(mutedLabel(
        QStringLiteral("标红项需要决定：修正后收录，或不收录。其余题目已自动收录。")));
    reviewTree_ = new QTreeWidget;
    reviewTree_->header()->setObjectName(QStringLiteral("reviewTreeHeader"));
    reviewTree_->setColumnCount(2);
    reviewTree_->setHeaderLabels({QStringLiteral("材料 / 题目"), QStringLiteral("问题")});
    reviewTree_->setTextElideMode(Qt::ElideMiddle);
    reviewTree_->header()->setStretchLastSection(true);
    reviewTree_->header()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    reviewTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    reviewTree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    reviewTree_->setAlternatingRowColors(false);
    reviewTree_->setRootIsDecorated(true);
    navigatorLayout->addWidget(reviewTree_, 1);
    reviewSplit->addWidget(navigator);

    auto* detail = new QFrame;
    detail->setObjectName(QStringLiteral("panel"));
    auto* detailLayout = new QVBoxLayout(detail);
    detailLayout->setContentsMargins(18, 16, 18, 16);
    detailLayout->setSpacing(10);
    reviewDetailTitle_ = new QLabel(QStringLiteral("选择左侧题目以预览"));
    reviewDetailTitle_->setObjectName(QStringLiteral("sectionTitle"));
    reviewDetailStatus_ = mutedLabel(
        QStringLiteral("待复核原因、完整题干和答案会显示在这里。"));
    reviewDetailStatus_->setWordWrap(true);
    detailLayout->addWidget(reviewDetailTitle_);
    detailLayout->addWidget(reviewDetailStatus_);
    reviewVisualPanel_ = new QWidget;
    reviewVisualLayout_ = new QVBoxLayout(reviewVisualPanel_);
    reviewVisualLayout_->setContentsMargins(0, 0, 0, 0);
    reviewVisualLayout_->setSpacing(8);
    reviewVisualPanel_->setVisible(false);
    detailLayout->addWidget(reviewVisualPanel_);
    reviewStemLabel_ = new QLabel(QStringLiteral("题干"));
    detailLayout->addWidget(reviewStemLabel_);
    reviewStemEditor_ = new QTextEdit;
    reviewStemEditor_->setObjectName(QStringLiteral("reviewStemEditor"));
    reviewStemEditor_->setPlaceholderText(QStringLiteral("题干"));
    reviewStemEditor_->setMinimumHeight(58);
    reviewStemEditor_->setMaximumHeight(240);
    reviewStemEditor_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    detailLayout->addWidget(reviewStemEditor_);
    manualMaterialUnderlineButton_ = new QPushButton(QStringLiteral("修正下划线 / 填空"));
    manualMaterialUnderlineButton_->setObjectName(QStringLiteral("secondaryButton"));
    manualMaterialUnderlineButton_->setVisible(false);
    detailLayout->addWidget(manualMaterialUnderlineButton_, 0, Qt::AlignLeft);
    reviewQuestionEditorPanel_ = new QWidget;
    auto* questionEditorLayout = new QVBoxLayout(reviewQuestionEditorPanel_);
    questionEditorLayout->setContentsMargins(0, 0, 0, 0);
    questionEditorLayout->setSpacing(10);
    auto* optionsHeader = new QHBoxLayout;
    optionsHeader->addWidget(new QLabel(QStringLiteral("选项")));
    optionsHeader->addStretch();
    auto* addOptionButton = new QPushButton(QStringLiteral("＋ 添加选项"));
    addOptionButton->setObjectName(QStringLiteral("secondaryButton"));
    optionsHeader->addWidget(addOptionButton);
    questionEditorLayout->addLayout(optionsHeader);
    reviewOptionsPanel_ = new QWidget;
    reviewOptionsLayout_ = new QVBoxLayout(reviewOptionsPanel_);
    reviewOptionsLayout_->setContentsMargins(0, 0, 0, 0);
    reviewOptionsLayout_->setSpacing(6);
    questionEditorLayout->addWidget(reviewOptionsPanel_);
    reviewAnswerLabel_ = new QLabel(QStringLiteral("正确答案"));
    questionEditorLayout->addWidget(reviewAnswerLabel_);
    reviewAnswerEditor_ = new QLineEdit;
    reviewAnswerEditor_->setPlaceholderText(QStringLiteral("例如 A；多选题填写 A、B"));
    questionEditorLayout->addWidget(reviewAnswerEditor_);
    reviewSolutionLabel_ = new QLabel(QStringLiteral("解析（可留空）"));
    questionEditorLayout->addWidget(reviewSolutionLabel_);
    reviewSolutionEditor_ = new QPlainTextEdit;
    reviewSolutionEditor_->setPlaceholderText(QStringLiteral("解析"));
    reviewSolutionEditor_->setMinimumHeight(80);
    questionEditorLayout->addWidget(reviewSolutionEditor_);
    auto* actions = new QHBoxLayout;
    confirmReviewButton_ = new QPushButton(QStringLiteral("保存并收录"));
    confirmReviewButton_->setObjectName(QStringLiteral("primaryButton"));
    excludeReviewButton_ = new QPushButton(QStringLiteral("不收录本题"));
    excludeReviewButton_->setObjectName(QStringLiteral("dangerTextButton"));
    actions->addWidget(excludeReviewButton_);
    actions->addStretch();
    actions->addWidget(confirmReviewButton_);
    questionEditorLayout->addLayout(actions);
    detailLayout->addWidget(reviewQuestionEditorPanel_);
    auto* detailScroll = new QScrollArea;
    detailScroll->setObjectName(QStringLiteral("reviewDetailScroll"));
    detailScroll->setWidgetResizable(true);
    detailScroll->setFrameShape(QFrame::NoFrame);
    detailScroll->setWidget(detail);
    reviewSplit->addWidget(detailScroll);
    reviewSplit->setStretchFactor(0, 4);
    reviewSplit->setStretchFactor(1, 6);
    reviewSplit->setSizes({360, 540});
    layout->addWidget(reviewSplit, 1);
    QObject::connect(reviewTree_, &QTreeWidget::currentItemChanged, page,
            [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
                if (currentReviewItem_ && currentReviewItem_ != current &&
                    !commitOpenReviewQuestion(QStringLiteral("切换题目"))) {
                    reviewTree_->blockSignals(true);
                    reviewTree_->setCurrentItem(currentReviewItem_);
                    reviewTree_->blockSignals(false);
                    return;
                }
                showReviewQuestion(current);
            });
    QObject::connect(confirmReviewButton_, &QPushButton::clicked, page,
            [this] { confirmCurrentReviewQuestion(); });
    QObject::connect(excludeReviewButton_, &QPushButton::clicked, page,
            [this] { excludeCurrentReviewQuestion(); });
    QObject::connect(addOptionButton, &QPushButton::clicked, page, [this] { addReviewOption(); });
    QObject::connect(reviewStemEditor_, &QTextEdit::textChanged,
            page, [this] { updateReviewStemHeight(); });
    QObject::connect(manualMaterialUnderlineButton_, &QPushButton::clicked, page,
            [this] { addManualMaterialUnderline(); });
    confirmReviewButton_->setEnabled(false);
    excludeReviewButton_->setEnabled(false);
    return page;
}

void ReviewPageController::populateReview(const GeneratedBankCandidate& candidate) {
    struct RestoreUpdates {
        QWidget* widget;
        bool enabled;
        ~RestoreUpdates() { widget->setUpdatesEnabled(enabled); }
    } restore{reviewTree_, reviewTree_->updatesEnabled()};
    reviewTree_->setUpdatesEnabled(false);
    diagnostic::event(QStringLiteral("studio"), QStringLiteral("review-populated"),
        {{QStringLiteral("materials"), candidate.materials.size()},
         {QStringLiteral("accepted"), candidate.questions.size()},
         {QStringLiteral("needsReview"), candidate.needsReviewQuestions.size()}});
#ifdef QUIZPANE_VERBOSE_DIAGNOSTICS
    diagnostic::payload(QStringLiteral("studio"), QStringLiteral("candidate"),
        QStringLiteral("materials-and-questions"),
        QString::fromUtf8(QJsonDocument(QJsonObject{
            {QStringLiteral("materials"), candidate.materials},
            {QStringLiteral("questions"), candidate.questions},
            {QStringLiteral("needsReviewQuestions"), candidate.needsReviewQuestions}})
                .toJson(QJsonDocument::Compact)),
        128 * 1024);
#endif
    lazyReviewAssets_.clear();
    reviewPdfCache_.clear();
    generatedMaterials_ = candidate.materials;
    generatedQuestions_ = candidate.questions;
    reviewQuestions_ = candidate.needsReviewQuestions;
    generatedAssets_ = candidate.assets;
    reviewSourceImages_ = candidate.reviewSourceImages;
    reviewAssets_ = candidate.reviewAssets;
    generatedHasAnswerKey_ = candidate.hasAnswerKey;
    activeReviewFilter_.clear();
    allQuestionsButton_->setChecked(true);
    currentReviewItem_ = nullptr;
    currentMaterialItem_ = nullptr;
    reviewTree_->clear();
    int hardReviewCount = 0;
    int automaticallyIncludedCount = 0;
    QHash<QString, QTreeWidgetItem*> groups;
    for (const auto& value : generatedMaterials_) {
        const QJsonObject material = value.toObject();
        const QString id = material.value("id").toString();
        const QString title = material.value("title").toString(id);
        const QJsonObject review = material.value("review").toObject();
        const bool isSoftRisk = review.value("needsReview").toBool() &&
                                review.value("riskLevel").toString() == QStringLiteral("soft");
        QStringList signalList;
        for (const QJsonValue& signal : review.value("signals").toArray())
            signalList.append(signal.toString());
        auto* item = new QTreeWidgetItem(reviewTree_, {title,
            isSoftRisk ? QStringLiteral("资料待复核") : QStringLiteral("共享材料")});
        item->setData(0, Qt::UserRole, material);
        item->setData(0, Qt::UserRole + 1, signalList);
        item->setData(0, Qt::UserRole + 2, false);
        item->setData(0, Qt::UserRole + 3, isSoftRisk);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
        item->setCheckState(0, Qt::Checked);
        if (isSoftRisk) item->setForeground(1, QColor(QStringLiteral("#d9a441")));
        groups.insert(id, item);
    }
    auto* independent = new QTreeWidgetItem(reviewTree_,
        {QStringLiteral("独立题目"), QStringLiteral("不引用共享材料")});
    independent->setFlags(independent->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
    independent->setCheckState(0, Qt::Checked);
    QTreeWidgetItem* brokenReferences = nullptr;

    int missingAnswers = 0;
    int duplicates = 0;
    const auto appendQuestions = [&](const QJsonArray& questions) {
        for (const auto& value : questions) {
            const QJsonObject question = value.toObject();
            const QJsonObject review = question.value("review").toObject();
            const bool needsReview = review.value("needsReview").toBool();
            const QString riskLevel = review.value("riskLevel").toString();
            const bool isHardRisk = needsReview && riskLevel != QStringLiteral("soft");
            const bool isSoftRisk = needsReview && riskLevel == QStringLiteral("soft");
            if (isHardRisk) ++hardReviewCount;
            else ++automaticallyIncludedCount;
            const QString reason = review.value("reason").toString();
            if (isHardRisk && reason.contains(QStringLiteral("答案"))) ++missingAnswers;
            if (isHardRisk && reason.contains(QStringLiteral("重复"))) ++duplicates;
            QStringList signalList;
            for (const auto& signal : review.value("signals").toArray())
                signalList.append(signal.toString());
            const QString materialId = question.value("materialId").toString();
            QTreeWidgetItem* parent = independent;
            if (!materialId.isEmpty()) {
                parent = groups.value(materialId, nullptr);
                if (!parent) {
                    if (!brokenReferences) {
                        brokenReferences = new QTreeWidgetItem(reviewTree_,
                            {QStringLiteral("引用断裂"), QStringLiteral("必须丢弃或修正")});
                        brokenReferences->setFlags(brokenReferences->flags() |
                            Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
                        brokenReferences->setCheckState(0, Qt::Checked);
                    }
                    parent = brokenReferences;
                }
            }
            QString statusText;
            if (isHardRisk) statusText = reason.left(240);
            else if (isSoftRisk) {
                statusText.clear();
            } else {
                statusText.clear();
            }
            const QString questionLabel = reviewQuestionTitle(question);
            auto* item = new QTreeWidgetItem(parent, {questionLabel, statusText});
            item->setToolTip(0, questionLabel);
            item->setTextAlignment(0, Qt::AlignLeft | Qt::AlignVCenter);
            item->setTextAlignment(1, Qt::AlignLeft | Qt::AlignVCenter);
            item->setData(0, Qt::UserRole, question);
            item->setData(0, Qt::UserRole + 1, signalList);
            item->setData(0, Qt::UserRole + 2, isHardRisk);
            item->setData(0, Qt::UserRole + 3, isSoftRisk);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(0, isHardRisk ? Qt::Unchecked : Qt::Checked);
            if (isHardRisk) item->setForeground(1, QColor(QStringLiteral("#c94b4b")));
        }
    };
    appendQuestions(generatedQuestions_);
    appendQuestions(reviewQuestions_);
    if (independent->childCount() == 0) delete independent;
    reviewTree_->expandToDepth(0);
    allQuestionsButton_->setText(QStringLiteral("全部题目  %1")
        .arg(generatedQuestions_.size() + reviewQuestions_.size()));
    allReviewButton_->setText(QStringLiteral("需要处理  %1").arg(hardReviewCount));
    allReviewButton_->setToolTip(QStringLiteral("只显示会影响题库内容、需要你决定的问题。"));
    missingAnswerButton_->setText(QStringLiteral("缺少答案  %1").arg(missingAnswers));
    missingAnswerButton_->setVisible(generatedHasAnswerKey_);
    duplicateButton_->setText(QStringLiteral("疑似重复  %1").arg(duplicates));

    clearLayout(riskCategoryLayout_);
    reviewSummary_ = new QLabel;
    reviewSummary_->setObjectName(QStringLiteral("reviewSummary"));
    reviewSummary_->setWordWrap(true);
    if (hardReviewCount == 0) {
        reviewSummary_->setText(QStringLiteral("✓ 已自动收录 %1 项，没有必须处理的问题，可直接继续。")
            .arg(automaticallyIncludedCount));
    } else {
        reviewSummary_->setText(QStringLiteral(
            "已自动收录 %1 项；还有 %2 项需要决定。未处理的题不会收录，不影响继续。")
            .arg(automaticallyIncludedCount).arg(hardReviewCount));
    }
    riskCategoryLayout_->addWidget(reviewSummary_);
    riskCategoryPanel_->setVisible(true);
    missingAnswerButton_->setVisible(false);
    duplicateButton_->setVisible(false);
    activeReviewFilter_ = hardReviewCount > 0 ? QStringLiteral("__any_review__") : QString();
    applyReviewFilter();
    if (onUpdateNavigation_) onUpdateNavigation_(hardReviewCount);
}

QByteArray ReviewPageController::ensureReviewAssetBytes(const QJsonObject& asset) {
    const QString path = asset.value(QStringLiteral("path")).toString();
    if (path.isEmpty())
        return {};
    if (reviewAssets_.contains(path))
        return reviewAssets_.value(path);
    if (generatedAssets_.contains(path))
        return generatedAssets_.value(path);
    if (!asset.value(QStringLiteral("lazyReview")).toBool())
        return {};

    const QString documentName = asset.value(QStringLiteral("sourceDocument")).toString();
    QString sourcePath;
    if (sourcePaths_) {
        for (const QString& candidate : *sourcePaths_) {
            if (QFileInfo(candidate).fileName() == documentName) {
                sourcePath = candidate;
                break;
            }
        }
    }
    const QString identity = ReviewPdfCache::sourceIdentity(sourcePath);
    if (identity.isEmpty())
        return {};
    const QString cacheKey = identity + QChar('\n') +
        QString::fromUtf8(QJsonDocument(asset).toJson(QJsonDocument::Compact));
    if (const auto* cached = lazyReviewAssets_.object(cacheKey))
        return *cached;

    QJsonArray segments = asset.value(QStringLiteral("reviewSegments")).toArray();
    if (segments.isEmpty()) {
        segments.append(QJsonObject{
            {QStringLiteral("sourcePage"), asset.value(QStringLiteral("sourcePage"))},
            {QStringLiteral("crop"), asset.value(QStringLiteral("autoCrop"))}});
    }
    QList<QImage> pieces;
    int width = 0;
    int height = 0;
    for (const QJsonValue& value : segments) {
        const QJsonObject segment = value.toObject();
        QString error;
        const QImage page = reviewPdfCache_.renderPage(
            sourcePath, segment.value(QStringLiteral("sourcePage")).toInt(), &error);
        if (page.isNull())
            return {};
        const QRectF crop = cropRectFromJson(segment.value(QStringLiteral("crop")).toObject());
        const QImage piece = cropNormalizedImage(
            page, crop.isEmpty() ? QRectF(0.0, 0.0, 1.0, 1.0) : crop);
        if (piece.isNull())
            return {};
        pieces.append(piece);
        width = qMax(width, piece.width());
        if (piece.height() > (std::numeric_limits<int>::max)() - height)
            return {};
        height += piece.height();
    }
    if (pieces.isEmpty())
        return {};
    if (!pdfImageAllocationAllowed(QSize(width, height)))
        return {};
    QImage combined(width, height, QImage::Format_RGB32);
    if (combined.isNull())
        return {};
    combined.fill(Qt::white);
    QPainter painter(&combined);
    int y = 0;
    for (const QImage& piece : pieces) {
        painter.drawImage(0, y, piece);
        y += piece.height();
    }
    painter.end();
    QByteArray png;
    QBuffer buffer(&png);
    if (!buffer.open(QIODevice::WriteOnly) || !combined.save(&buffer, "PNG"))
        return {};
    lazyReviewAssets_.insert(cacheKey, new QByteArray(png), int((png.size() + 1023) / 1024));
    diagnostic::event(QStringLiteral("studio"), QStringLiteral("review-image-lazy-rendered"),
        {{QStringLiteral("segments"), segments.size()},
         {QStringLiteral("pngBytes"), png.size()}});
    return png;
}

void ReviewPageController::displayReviewAssets(const QList<QJsonObject>& assets) {
    clearLayout(reviewVisualLayout_);
    QList<QJsonObject> validAssets;
    for (const QJsonObject& asset : assets) {
        const QString path = asset.value(QStringLiteral("path")).toString();
        const QByteArray bytes = ensureReviewAssetBytes(asset);
        QPixmap pixmap;
        if (path.isEmpty() || bytes.isEmpty() || !pixmap.loadFromData(bytes, "PNG"))
            continue;
        validAssets.append(asset);
    }
    if (validAssets.isEmpty()) {
        if (!assets.isEmpty()) {
            auto* message = mutedLabel(QStringLiteral(
                "原卷图片暂时无法显示，请确认源文件仍在原位置，或关闭其他程序后重新选择本题。"));
            message->setWordWrap(true);
            reviewVisualLayout_->addWidget(message);
        }
        reviewVisualPanel_->setVisible(!assets.isEmpty());
        return;
    }

    auto* titleRow = new QHBoxLayout;
    auto* title = mutedLabel(validAssets.first().value(QStringLiteral("alt")).toString());
    titleRow->addWidget(title);
    auto* recrop = new QPushButton(QStringLiteral("手动修正"));
    recrop->setObjectName(QStringLiteral("reviewActionButton"));
    titleRow->addWidget(recrop);
    QComboBox* picker = nullptr;
    if (validAssets.size() > 1) {
        picker = new QComboBox;
        for (int index = 0; index < validAssets.size(); ++index) {
            const int sourcePage = validAssets.at(index).value(QStringLiteral("sourcePage")).toInt();
            picker->addItem(sourcePage > 0
                ? QStringLiteral("原卷第 %1 页").arg(sourcePage)
                : QStringLiteral("图片 %1").arg(index + 1));
        }
        titleRow->addWidget(picker);
    }
    titleRow->addStretch(1);
    reviewVisualLayout_->addLayout(titleRow);

    auto* image = new QLabel;
    image->setAlignment(Qt::AlignCenter);
    reviewVisualLayout_->addWidget(image);
    const auto selectedAsset = [validAssets, picker] {
        return validAssets.at(picker ? picker->currentIndex() : 0);
    };
    const auto showAsset = [this, validAssets, title, image, recrop](int index) {
        const QJsonObject asset = validAssets.at(index);
        QPixmap pixmap;
        const QByteArray bytes = ensureReviewAssetBytes(asset);
        pixmap.loadFromData(bytes, "PNG");
        image->setPixmap(pixmap.scaledToWidth(520, Qt::SmoothTransformation));
        image->setToolTip(asset.value(QStringLiteral("path")).toString());
        title->setText(asset.value(QStringLiteral("alt")).toString());
        const bool canRecrop = asset.value(QStringLiteral("sourcePage")).toInt() > 0 &&
            !asset.value(QStringLiteral("sourceDocument")).toString().isEmpty();
        recrop->setText(asset.value(QStringLiteral("reviewOnly")).toBool()
            ? QStringLiteral("调整原卷区域") : QStringLiteral("手动修正"));
        recrop->setVisible(canRecrop);
        recrop->setEnabled(canRecrop);
    };
    showAsset(0);
    if (picker)
        QObject::connect(picker, qOverload<int>(&QComboBox::currentIndexChanged), image, showAsset);
    QObject::connect(recrop, &QPushButton::clicked, image,
            [this, selectedAsset] { recropReviewAsset(selectedAsset()); });
    reviewVisualPanel_->setVisible(true);
}

void ReviewPageController::recropReviewAsset(const QJsonObject& asset) {
    const QString documentName = asset.value(QStringLiteral("sourceDocument")).toString();
    const int page = asset.value(QStringLiteral("sourcePage")).toInt();
    QString sourcePath;
    if (sourcePaths_) {
        for (const QString& candidate : *sourcePaths_) {
            if (QFileInfo(candidate).fileName() == documentName) {
                sourcePath = candidate;
                break;
            }
        }
    }
    if (sourcePath.isEmpty()) {
        QMessageBox::warning(parentWidget_, QStringLiteral("找不到原卷"),
            QStringLiteral("找不到“%1”。请重新添加原题 PDF 后再次整理。").arg(documentName));
        return;
    }
    QString error;
    const QImage pageImage = reviewPdfCache_.renderPage(sourcePath, page, &error);
    if (pageImage.isNull()) {
        QMessageBox::warning(parentWidget_, QStringLiteral("无法重新裁切"), error);
        return;
    }
    const QRectF automaticCrop = cropRectFromJson(asset.value(QStringLiteral("autoCrop")).toObject());
    const QRectF initialCrop = cropRectFromJson(asset.value(QStringLiteral("crop")).toObject());
    const QRectF fallbackCrop(0.05, 0.05, 0.90, 0.90);
    const QRectF automatic = automaticCrop.isEmpty() ? fallbackCrop : automaticCrop;
    const QRectF contextAnchor = initialCrop.isEmpty() ? automatic : initialCrop;
    CropDialog dialog(pageImage, cropContextAround(contextAnchor), contextAnchor, parentWidget_);
    if (!initialCrop.isEmpty())
        dialog.setSelection(initialCrop);
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (commitReviewCrop(asset, pageImage, dialog.selection()))
        reviewDetailStatus_->setText(QStringLiteral("已从原卷第 %1 页更新截图；保存草稿后再确认本题。").arg(page));
}

bool ReviewPageController::commitReviewCrop(const QJsonObject& asset, const QImage& pageImage,
                                             const QRectF& normalizedCrop) {
    const QRect crop(qFloor(normalizedCrop.x() * pageImage.width()),
                     qFloor(normalizedCrop.y() * pageImage.height()),
                     qMax(1, qCeil(normalizedCrop.width() * pageImage.width())),
                     qMax(1, qCeil(normalizedCrop.height() * pageImage.height())));
    const QImage clipped = pageImage.copy(crop.intersected(pageImage.rect()));
    QByteArray png;
    QBuffer buffer(&png);
    if (clipped.isNull() || !buffer.open(QIODevice::WriteOnly) || !clipped.save(&buffer, "PNG")) {
        QMessageBox::warning(parentWidget_, QStringLiteral("无法重新裁切"), QStringLiteral("无法保存新截图。"));
        return false;
    }
    const QString path = asset.value(QStringLiteral("path")).toString();
    if (path.isEmpty())
        return false;
    QJsonObject replacement = asset;
    replacement.insert(QStringLiteral("crop"), cropRectToJson(normalizedCrop));
    QTreeWidgetItem* item = reviewTree_->currentItem();
    if (!item) return false;
    QJsonObject entry = item->data(0, Qt::UserRole).toJsonObject();
    if (asset.value(QStringLiteral("reviewOnly")).toBool()) {
        const QString id = entry.value(QStringLiteral("id")).toString();
        if (id.isEmpty())
            return false;
        reviewAssets_.insert(path, png);
        reviewSourceImages_.insert(id, replacement);
        showReviewQuestion(item);
        return true;
    }
    generatedAssets_.insert(path, png);
    const auto replaceAsset = [&replacement, &path](QJsonObject* owner, const QString& key) {
        QJsonObject image = owner->value(key).toObject();
        if (image.value(QStringLiteral("path")).toString() != path)
            return false;
        owner->insert(key, replacement);
        return true;
    };
    if (entry.contains(QStringLiteral("body"))) {
        QJsonArray images = entry.value(QStringLiteral("images")).toArray();
        for (int index = 0; index < images.size(); ++index) {
            if (images.at(index).toObject().value(QStringLiteral("path")).toString() == path)
                images[index] = replacement;
        }
        entry.insert(QStringLiteral("images"), images);
        const QString id = entry.value(QStringLiteral("id")).toString();
        for (int index = 0; index < generatedMaterials_.size(); ++index) {
            if (generatedMaterials_.at(index).toObject().value(QStringLiteral("id")).toString() == id)
                generatedMaterials_[index] = entry;
        }
    } else {
        replaceAsset(&entry, QStringLiteral("stemImage"));
        QJsonArray options = entry.value(QStringLiteral("options")).toArray();
        for (int index = 0; index < options.size(); ++index) {
            QJsonObject option = options.at(index).toObject();
            if (replaceAsset(&option, QStringLiteral("image")))
                options[index] = option;
        }
        entry.insert(QStringLiteral("options"), options);
    }
    item->setData(0, Qt::UserRole, entry);
    showReviewQuestion(item);
    return true;
}

void ReviewPageController::setReviewOptions(const QJsonArray& options) {
    QLayoutItem* child;
    while ((child = reviewOptionsLayout_->takeAt(0)) != nullptr) {
        delete child->widget();
        delete child;
    }
    reviewOptionEditors_.clear();
    for (const QJsonValue& value : options) {
        const QJsonObject option = value.toObject();
        addReviewOption(option.value(QStringLiteral("id")).toString(),
                        option.value(QStringLiteral("text")).toString());
    }
}

void ReviewPageController::addReviewOption(const QString& requestedId, const QString& text) {
    QString id = requestedId.trimmed().toLower();
    if (id.isEmpty()) {
        for (int index = 0; index < 26; ++index) {
            const QString candidate(QChar(u'a' + index));
            const bool used = std::any_of(reviewOptionEditors_.cbegin(), reviewOptionEditors_.cend(),
                [&candidate](const QLineEdit* editor) {
                    return editor->property("optionId").toString() == candidate;
                });
            if (!used) {
                id = candidate;
                break;
            }
        }
    }
    if (id.isEmpty())
        id = QStringLiteral("option-%1").arg(reviewOptionEditors_.size() + 1);
    auto* row = new QFrame;
    row->setObjectName(QStringLiteral("reviewOptionRow"));
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(8, 5, 6, 5);
    layout->setSpacing(8);
    auto* badge = new QLabel(id.toUpper());
    badge->setObjectName(QStringLiteral("reviewOptionId"));
    badge->setFixedWidth(22);
    badge->setAlignment(Qt::AlignCenter);
    auto* editor = new QLineEdit(text);
    editor->setPlaceholderText(QStringLiteral("选项内容"));
    editor->setProperty("optionId", id);
    auto* remove = new QPushButton(QStringLiteral("删除"));
    remove->setObjectName(QStringLiteral("reviewOptionRemoveButton"));
    remove->setToolTip(QStringLiteral("删除此选项"));
    remove->setFixedWidth(52);
    layout->addWidget(badge);
    layout->addWidget(editor, 1);
    layout->addWidget(remove);
    reviewOptionsLayout_->addWidget(row);
    reviewOptionEditors_.append(editor);
    QObject::connect(remove, &QPushButton::clicked, row, [this, editor, row] {
        reviewOptionEditors_.removeAll(editor);
        reviewOptionsLayout_->removeWidget(row);
        row->deleteLater();
    });
}

QJsonArray ReviewPageController::reviewOptions() const {
    QJsonArray result;
    for (const QLineEdit* editor : reviewOptionEditors_) {
        const QString text = editor->text().trimmed();
        if (text.isEmpty())
            return {};
        const QString id = editor->property("optionId").toString();
        result.append(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("text"), text}});
    }
    return result;
}

void ReviewPageController::showReviewQuestion(QTreeWidgetItem* item) {
    reviewStemEditor_->setExtraSelections({});
    const QJsonObject entry = item ? item->data(0, Qt::UserRole).toJsonObject() : QJsonObject{};
    const bool isMaterial = entry.contains(QStringLiteral("body"));
    currentReviewItem_ = entry.isEmpty() || isMaterial ? nullptr : item;
    currentMaterialItem_ = isMaterial ? item : nullptr;
    const bool available = currentReviewItem_ != nullptr;
    confirmReviewButton_->setEnabled(available);
    excludeReviewButton_->setEnabled(available);
    if (!available) {
        reviewStemEditor_->setReadOnly(isMaterial);
        reviewQuestionEditorPanel_->setVisible(false);
        reviewAnswerEditor_->setReadOnly(isMaterial);
        reviewSolutionEditor_->setReadOnly(isMaterial);
        if (isMaterial) {
            const QStringList reviewSignals = item->data(0, Qt::UserRole + 1).toStringList();
            manualMaterialUnderlineButton_->setVisible(
                reviewSignals.contains(QStringLiteral("material-layout:underline-or-blank")) ||
                entry.value(QStringLiteral("body")).toString().contains(QStringLiteral("〔填空〕")) ||
                !entry.value(QStringLiteral("underlines")).toArray().isEmpty());
            reviewStemLabel_->setText(QStringLiteral("材料文本"));
            reviewDetailTitle_->setText(QStringLiteral("共享材料：%1")
                .arg(entry.value(QStringLiteral("title")).toString()));
            reviewDetailStatus_->setText(QStringLiteral(
                "这是原始共享材料。下方先显示保留版式的原卷截图，便于核对下划线、填空和图表。"));
            reviewStemEditor_->setHtml(materialPreviewHtml(
                entry.value(QStringLiteral("body")).toString(),
                entry.value(QStringLiteral("underlines")).toArray()));
            updateReviewStemHeight();
            setReviewOptions({});
            reviewAnswerEditor_->clear();
            reviewSolutionEditor_->clear();
            QList<QJsonObject> images;
            for (const QJsonValue& value : entry.value(QStringLiteral("images")).toArray())
                images.append(value.toObject());
            displayReviewAssets(images);
            return;
        }
        manualMaterialUnderlineButton_->setVisible(false);
        reviewDetailTitle_->setText(QStringLiteral("选择左侧题目以预览"));
        reviewDetailStatus_->setText(QStringLiteral("待复核原因、完整题干和答案会显示在这里。"));
        reviewStemLabel_->setText(QStringLiteral("题干"));
        reviewStemEditor_->clear();
        updateReviewStemHeight();
        setReviewOptions({});
        reviewAnswerEditor_->clear();
        reviewSolutionEditor_->clear();
        displayReviewAssets({});
        return;
    }

    const QStringList reviewSignals = item->data(0, Qt::UserRole + 1).toStringList();
    manualMaterialUnderlineButton_->setVisible(
        reviewSignals.contains(QStringLiteral("stem-layout:underline-or-blank")) ||
        entry.value(QStringLiteral("stem")).toString().contains(QStringLiteral("〔填空〕")) ||
        !entry.value(QStringLiteral("stemUnderlines")).toArray().isEmpty());
    reviewStemEditor_->setReadOnly(false);
    reviewStemLabel_->setText(QStringLiteral("题干"));
    reviewQuestionEditorPanel_->setVisible(true);
    reviewAnswerLabel_->setVisible(generatedHasAnswerKey_);
    reviewAnswerEditor_->setVisible(generatedHasAnswerKey_);
    reviewSolutionLabel_->setVisible(generatedHasAnswerKey_);
    reviewSolutionEditor_->setVisible(generatedHasAnswerKey_);
    reviewAnswerEditor_->setReadOnly(false);
    reviewSolutionEditor_->setReadOnly(false);
    const QJsonObject question = entry;
    reviewDetailTitle_->setText(reviewQuestionTitle(question));
    const QJsonObject review = question.value("review").toObject();
    QString status;
    const bool hardRisk = item->data(0, Qt::UserRole + 2).toBool();
    const bool softRisk = item->data(0, Qt::UserRole + 3).toBool();
    if (hardRisk)
        status = QStringLiteral("需要处理：%1").arg(review.value("reason").toString());
    else if (softRisk)
        status = QStringLiteral("已自动收录。图片、扫描内容和版式已处理；如有偏差可在下方修改。");
    else
        status = QStringLiteral("已自动收录，可直接继续。");
    reviewDetailStatus_->setText(status);
    confirmReviewButton_->setText(hardRisk ? QStringLiteral("保存并收录")
                                           : QStringLiteral("保存修改"));
    reviewStemEditor_->setPlainText(stemForReviewEditor(question.value("stem").toString()));
    QList<QTextEdit::ExtraSelection> underlineSelections;
    for (const auto& value : question.value("stemUnderlines").toArray()) {
        const auto range = value.toObject();
        QTextEdit::ExtraSelection selection;
        selection.cursor = reviewStemEditor_->textCursor();
        selection.cursor.setPosition(range.value("start").toInt());
        selection.cursor.setPosition(range.value("start").toInt() + range.value("length").toInt(), QTextCursor::KeepAnchor);
        selection.format.setFontUnderline(true);
        underlineSelections.append(selection);
    }
    reviewStemEditor_->setExtraSelections(underlineSelections);
    updateReviewStemHeight();
    setReviewOptions(question.value("options").toArray());
    if (generatedHasAnswerKey_) {
        QStringList answerIds;
        for (const QJsonValue& value : question.value("answer").toObject().value("optionIds").toArray())
            answerIds.append(value.toString());
        reviewAnswerEditor_->setText(answerIds.join(QStringLiteral(", ")));
        reviewSolutionEditor_->setPlainText(question.value("solution").toString());
    } else {
        reviewAnswerEditor_->clear();
        reviewSolutionEditor_->clear();
    }
    QList<QJsonObject> images;
    QSet<QString> imagePaths;
    const auto appendImage = [&images, &imagePaths](const QJsonObject& image) {
        const QString path = image.value(QStringLiteral("path")).toString();
        if (image.isEmpty() || path.isEmpty() || imagePaths.contains(path)) return;
        imagePaths.insert(path);
        images.append(image);
    };
    appendImage(reviewSourceImages_.value(question.value(QStringLiteral("id")).toString()));
    const QJsonObject stemImage = question.value(QStringLiteral("stemImage")).toObject();
    appendImage(stemImage);
    for (const QJsonValue& value : question.value(QStringLiteral("options")).toArray()) {
        const QJsonObject image = value.toObject().value(QStringLiteral("image")).toObject();
        appendImage(image);
    }
    displayReviewAssets(images);
}

void ReviewPageController::addManualMaterialUnderline() {
    QTreeWidgetItem* target = currentMaterialItem_ ? currentMaterialItem_ : currentReviewItem_;
    if (!target) return;
    if (currentReviewItem_ && reviewQuestionIsDirty()) {
        QMessageBox::information(parentWidget_, QStringLiteral("请先保存修改"),
            QStringLiteral("请先保存题干和选项的修改，再标记下划线，以免位置发生偏移。"));
        return;
    }
    const bool isMaterial = currentMaterialItem_ != nullptr;
    const QString underlineKey = isMaterial ? QStringLiteral("underlines") : QStringLiteral("stemUnderlines");
    QJsonObject material = target->data(0, Qt::UserRole).toJsonObject();
    const QString body = material.value(isMaterial ? QStringLiteral("body") : QStringLiteral("stem")).toString();
    if (body.isEmpty())
        return;

    QDialog dialog(parentWidget_);
    dialog.setWindowTitle(QStringLiteral("修正下划线 / 填空"));
    dialog.setMinimumSize(620, 420);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->addWidget(mutedLabel(QStringLiteral(
        "如果文字被误写成“〔填空〕”，请先关闭此窗口并在题干中改回原文。"
        "然后选中需要带下划线的词句，点击“设为下划线”。")));
    auto* editor = new QPlainTextEdit;
    editor->setPlainText(body);
    editor->setReadOnly(true);
    editor->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    layout->addWidget(editor, 1);
    auto* buttons = new QDialogButtonBox;
    auto* add = buttons->addButton(QStringLiteral("设为下划线"), QDialogButtonBox::AcceptRole);
    add->setObjectName(QStringLiteral("primaryButton"));
    auto* cancel = buttons->addButton(QStringLiteral("取消"), QDialogButtonBox::RejectRole);
    auto* clear = buttons->addButton(QStringLiteral("清除全部下划线"), QDialogButtonBox::ResetRole);
    const auto saveRanges = [&](const QJsonArray& ranges) {
        material.insert(underlineKey, ranges);
        target->setData(0, Qt::UserRole, material);
        if (isMaterial) {
            const QString id = material.value(QStringLiteral("id")).toString();
            for (int index = 0; index < generatedMaterials_.size(); ++index)
                if (generatedMaterials_.at(index).toObject().value(QStringLiteral("id")).toString() == id) {
                    generatedMaterials_[index] = material;
                    break;
                }
        }
        dialog.accept();
    };
    QObject::connect(clear, &QPushButton::clicked, &dialog, [&] { saveRanges({}); });
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(add, &QPushButton::clicked, &dialog, [&] {
        const QTextCursor cursor = editor->textCursor();
        const int start = cursor.selectionStart();
        const int length = cursor.selectionEnd() - start;
        if (length <= 0) {
            QMessageBox::information(&dialog, QStringLiteral("请先选择文字"),
                QStringLiteral("请在文本中选中一个词或一句话。"));
            return;
        }
        QList<QPair<int, int>> ranges;
        for (const QJsonValue& value : material.value(underlineKey).toArray()) {
            const QJsonObject range = value.toObject();
            const int rangeStart = range.value(QStringLiteral("start")).toInt(-1);
            const int rangeLength = range.value(QStringLiteral("length")).toInt();
            if (rangeStart >= 0 && rangeLength > 0 && rangeStart + rangeLength <= body.size())
                ranges.append({rangeStart, rangeLength});
        }
        ranges.append({start, length});
        std::sort(ranges.begin(), ranges.end(), [](const auto& left, const auto& right) {
            return left.first < right.first;
        });
        QJsonArray merged;
        for (const auto& range : ranges) {
            if (merged.isEmpty()) {
                merged.append(QJsonObject{{QStringLiteral("start"), range.first},
                                          {QStringLiteral("length"), range.second}});
                continue;
            }
            QJsonObject previous = merged.last().toObject();
            const int previousEnd = previous.value(QStringLiteral("start")).toInt() +
                previous.value(QStringLiteral("length")).toInt();
            if (range.first <= previousEnd) {
                previous.insert(QStringLiteral("length"),
                    qMax(previousEnd, range.first + range.second) -
                    previous.value(QStringLiteral("start")).toInt());
                merged[merged.size() - 1] = previous;
            } else {
                merged.append(QJsonObject{{QStringLiteral("start"), range.first},
                                          {QStringLiteral("length"), range.second}});
            }
        }
        saveRanges(merged);
    });
    layout->addWidget(buttons);
    if (dialog.exec() == QDialog::Accepted)
        showReviewQuestion(target);
}

bool ReviewPageController::saveCurrentReviewQuestion() {
    if (!currentReviewItem_)
        return false;
    QJsonObject question = currentReviewItem_->data(0, Qt::UserRole).toJsonObject();
    const QString stem = stemFromReviewEditor(reviewStemEditor_->toPlainText()).trimmed();
    if (stem.isEmpty()) {
        QMessageBox::warning(parentWidget_, QStringLiteral("无法保存草稿"), QStringLiteral("题干不能为空。"));
        return false;
    }

    const QJsonArray options = reviewOptions();
    if (options.isEmpty()) {
        QMessageBox::warning(parentWidget_, QStringLiteral("无法保存草稿"), QStringLiteral("至少需要一个选项。"));
        return false;
    }
    if (stem != question.value(QStringLiteral("stem")).toString()) {
        if (!question.value(QStringLiteral("stemUnderlines")).toArray().isEmpty() &&
            QMessageBox::question(parentWidget_, QStringLiteral("请重新核对下划线"),
                QStringLiteral("题干已修改，原下划线位置可能失效。保存后将清除旧标记，"
                               "可用“手动标记下划线”重新设置。继续保存吗？"),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
            return false;
        question.remove(QStringLiteral("stemUnderlines"));
        reviewStemEditor_->setExtraSelections({});
    }
    question.insert(QStringLiteral("stem"), stem);
    question.insert(QStringLiteral("options"), options);
    if (generatedHasAnswerKey_) {
        QJsonArray answerIds;
        const QStringList rawAnswerIds = reviewAnswerEditor_->text().split(
            QRegularExpression(QStringLiteral("[,，\\s]+")), Qt::SkipEmptyParts);
        for (const QString& id : rawAnswerIds)
            answerIds.append(id.trimmed().toLower());
        if (answerIds.isEmpty()) {
            QMessageBox::warning(parentWidget_, QStringLiteral("无法保存草稿"), QStringLiteral("请填写答案选项 ID。"));
            return false;
        }
        question.insert(QStringLiteral("answer"), QJsonObject{{QStringLiteral("optionIds"), answerIds}});
        question.insert(QStringLiteral("solution"), reviewSolutionEditor_->toPlainText().trimmed());
    } else {
        question.remove(QStringLiteral("answer"));
        question.remove(QStringLiteral("solution"));
    }

    QJsonArray materials;
    const QString materialId = question.value("materialId").toString();
    if (!materialId.isEmpty()) {
        for (const QJsonValue& value : generatedMaterials_) {
            if (value.toObject().value("id").toString() == materialId) {
                materials.append(value);
                break;
            }
        }
    }
    const QJsonObject bank = makeReviewDraftBank(question, materials, generatedHasAnswerKey_);
    QString error;
    if (!quizpane::validateBank(bank, &error)) {
        QMessageBox::warning(parentWidget_, QStringLiteral("草稿尚不完整"),
                             QStringLiteral("请修正后再保存：%1").arg(error));
        return false;
    }
    currentReviewItem_->setData(0, Qt::UserRole, question);
    const bool needsReview = question.value("review").toObject().value("needsReview").toBool();
    currentReviewItem_->setText(1, needsReview
        ? QStringLiteral("草稿已保存 · 仍待确认") : QStringLiteral("已人工确认"));
    reviewDetailStatus_->setText(needsReview
        ? QStringLiteral("草稿已保存。确认本题后才会进入最终题库。")
        : QStringLiteral("已保存并确认，将进入最终题库。"));
    diagnostic::event(QStringLiteral("studio"), QStringLiteral("review-draft-saved"),
        {{QStringLiteral("questionId"), question.value("id").toString()}});
    return true;
}

bool ReviewPageController::reviewQuestionIsDirty() const {
    if (!currentReviewItem_)
        return false;
    const QJsonObject question = currentReviewItem_->data(0, Qt::UserRole).toJsonObject();
    if (question.isEmpty())
        return false;
    if (stemFromReviewEditor(reviewStemEditor_->toPlainText()).trimmed() !=
        question.value("stem").toString())
        return true;
    const QJsonArray options = reviewOptions();
    const QJsonArray savedOptions = question.value("options").toArray();
    if (options.size() != savedOptions.size())
        return true;
    for (int index = 0; index < options.size(); ++index) {
        const QJsonObject saved = savedOptions.at(index).toObject();
        const QJsonObject current = options.at(index).toObject();
        if (saved.value("id") != current.value("id") ||
            saved.value("text") != current.value("text"))
            return true;
    }
    if (generatedHasAnswerKey_) {
        QJsonArray editorAnswerIds;
        const QStringList rawAnswerIds = reviewAnswerEditor_->text().split(
            QRegularExpression(QStringLiteral("[,，\\s]+")), Qt::SkipEmptyParts);
        for (const QString& id : rawAnswerIds)
            editorAnswerIds.append(id.trimmed().toLower());
        const QJsonArray savedAnswerIds =
            question.value("answer").toObject().value("optionIds").toArray();
        if (editorAnswerIds != savedAnswerIds)
            return true;
        if (reviewSolutionEditor_->toPlainText().trimmed() !=
            question.value("solution").toString())
            return true;
    }
    return false;
}

bool ReviewPageController::commitOpenReviewQuestion(const QString& consequence) {
    if (!currentReviewItem_ || !reviewQuestionIsDirty())
        return true;
    if (saveCurrentReviewQuestion())
        return true;
    const QMessageBox::StandardButton choice = QMessageBox::question(
        parentWidget_, QStringLiteral("有未保存的修改"),
        QStringLiteral("当前题目的草稿无法保存，%1将丢弃未保存的修改。\n仍要继续吗？")
            .arg(consequence),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    return choice == QMessageBox::Yes;
}

void ReviewPageController::confirmCurrentReviewQuestion() {
    const bool wasHardRisk = currentReviewItem_ &&
        currentReviewItem_->data(0, Qt::UserRole + 2).toBool();
    if (!saveCurrentReviewQuestion())
        return;
    QJsonObject question = currentReviewItem_->data(0, Qt::UserRole).toJsonObject();
    QJsonObject review = question.value("review").toObject();
    review.insert(QStringLiteral("needsReview"), false);
    review.insert(QStringLiteral("reason"), QStringLiteral("已人工确认"));
    question.insert(QStringLiteral("review"), review);
    currentReviewItem_->setData(0, Qt::UserRole, question);
    currentReviewItem_->setData(0, Qt::UserRole + 2, false);
    currentReviewItem_->setData(0, Qt::UserRole + 3, false);
    currentReviewItem_->setCheckState(0, Qt::Checked);
    currentReviewItem_->setText(1, QStringLiteral("已人工确认"));
    reviewDetailStatus_->setText(QStringLiteral("已确认；它会进入最终题库。"));
    diagnostic::event(QStringLiteral("studio"), QStringLiteral("review-question-confirmed"),
        {{QStringLiteral("questionId"), question.value("id").toString()}});
    refreshReviewDecisionState();
    if (wasHardRisk)
        advanceToNextReviewIssue();
}

void ReviewPageController::excludeCurrentReviewQuestion() {
    if (!currentReviewItem_)
        return;
    currentReviewItem_->setCheckState(0, Qt::Unchecked);
    currentReviewItem_->setData(0, Qt::UserRole + 2, false);
    currentReviewItem_->setData(0, Qt::UserRole + 3, false);
    currentReviewItem_->setText(1, QStringLiteral("暂不采用"));
    reviewDetailStatus_->setText(QStringLiteral("本题不会进入最终题库；重新勾选或确认本题即可恢复。"));
    diagnostic::event(QStringLiteral("studio"), QStringLiteral("review-question-excluded"),
        {{QStringLiteral("questionId"), currentReviewItem_->data(0, Qt::UserRole)
            .toJsonObject().value("id").toString()}});
    refreshReviewDecisionState();
    advanceToNextReviewIssue();
}

void ReviewPageController::refreshReviewDecisionState() {
    int included = 0;
    int remaining = 0;
    for (QTreeWidgetItemIterator it(reviewTree_); *it; ++it) {
        QTreeWidgetItem* item = *it;
        const QJsonObject entry = item->data(0, Qt::UserRole).toJsonObject();
        if (entry.isEmpty() || entry.contains(QStringLiteral("body")))
            continue;
        if (item->data(0, Qt::UserRole + 2).toBool())
            ++remaining;
        else if (item->checkState(0) == Qt::Checked)
            ++included;
    }
    allReviewButton_->setText(QStringLiteral("需要处理  %1").arg(remaining));
    if (reviewSummary_) {
        reviewSummary_->setText(remaining == 0
            ? QStringLiteral("✓ 已收录 %1 题，没有待处理问题，可以继续生成题库。").arg(included)
            : QStringLiteral("已收录 %1 题；还有 %2 题需要决定。未处理的题不会收录。")
                  .arg(included).arg(remaining));
    }
    if (remaining == 0) {
        activeReviewFilter_.clear();
        allQuestionsButton_->setChecked(true);
    }
    applyReviewFilter();
    if (onUpdateNavigation_) onUpdateNavigation_(remaining);
}

void ReviewPageController::advanceToNextReviewIssue() {
    for (QTreeWidgetItemIterator it(reviewTree_); *it; ++it) {
        QTreeWidgetItem* item = *it;
        if (!item->isHidden() && item->data(0, Qt::UserRole + 2).toBool()) {
            reviewTree_->blockSignals(true);
            reviewTree_->setCurrentItem(item);
            reviewTree_->blockSignals(false);
            showReviewQuestion(item);
            return;
        }
    }
    if (currentReviewItem_) {
        reviewTree_->blockSignals(true);
        reviewTree_->setCurrentItem(currentReviewItem_);
        reviewTree_->blockSignals(false);
        showReviewQuestion(currentReviewItem_);
    }
}

void ReviewPageController::updateReviewStemHeight() {
    if (!reviewStemEditor_)
        return;
    const qreal documentHeight = reviewStemEditor_->document()->size().height();
    const int contentHeight = qCeil(documentHeight) + 18;
    constexpr int minimumHeight = 58;
    constexpr int maximumHeight = 240;
    const int height = qBound(minimumHeight, contentHeight, maximumHeight);
    reviewStemEditor_->setFixedHeight(height);
    reviewStemEditor_->setVerticalScrollBarPolicy(contentHeight > maximumHeight
        ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
}

void ReviewPageController::applyReviewFilter() {
    for (auto* button : reviewFilterGroup_->buttons()) {
        if (button->property("reviewFilter").toString() == activeReviewFilter_)
            button->setChecked(true);
    }
    std::function<bool(QTreeWidgetItem*)> visit = [&](QTreeWidgetItem* item) -> bool {
        const auto matches = [this](QTreeWidgetItem* candidate) {
            if (activeReviewFilter_.isEmpty()) return true;
            const bool hardRisk = candidate->data(0, Qt::UserRole + 2).toBool();
            const bool softRisk = candidate->data(0, Qt::UserRole + 3).toBool();
            if (activeReviewFilter_ == QStringLiteral("__any_review__"))
                return hardRisk;
            if (activeReviewFilter_ == QStringLiteral("__missing_answer__"))
                return hardRisk && candidate->text(1).contains(QStringLiteral("答案"));
            if (activeReviewFilter_ == QStringLiteral("__duplicate__"))
                return hardRisk && candidate->text(1).contains(QStringLiteral("重复"));
            static const QString signalPrefix = QStringLiteral("__signal:");
            if (activeReviewFilter_.startsWith(signalPrefix))
                return candidate->data(0, Qt::UserRole + 1).toStringList().contains(
                    activeReviewFilter_.mid(signalPrefix.size()));
            return true;
        };
        const bool selfMatches = matches(item);
        bool anyChildVisible = false;
        for (int index = 0; index < item->childCount(); ++index) {
            QTreeWidgetItem* child = item->child(index);
            if (visit(child)) anyChildVisible = true;
        }
        const bool visible = selfMatches || anyChildVisible;
        item->setHidden(!visible);
        return visible;
    };
    for (int index = 0; index < reviewTree_->topLevelItemCount(); ++index)
        visit(reviewTree_->topLevelItem(index));
}

void ReviewPageController::discardForNewTask() {
    const int generatedAssetCount = generatedAssets_.size();
    const int reviewAssetCount = reviewAssets_.size();
    qint64 generatedAssetBytes = 0;
    for (auto it = generatedAssets_.cbegin(); it != generatedAssets_.cend(); ++it)
        generatedAssetBytes += it.value().size();
    qint64 reviewAssetBytes = 0;
    for (auto it = reviewAssets_.cbegin(); it != reviewAssets_.cend(); ++it)
        reviewAssetBytes += it.value().size();

    QVariantMap before = diagnostic::memorySnapshot();
    before.insert(QStringLiteral("generatedAssetBytes"), generatedAssetBytes);
    before.insert(QStringLiteral("generatedAssets"), generatedAssetCount);
    before.insert(QStringLiteral("questions"),
                  generatedQuestions_.size() + reviewQuestions_.size());
    before.insert(QStringLiteral("reviewAssetBytes"), reviewAssetBytes);
    before.insert(QStringLiteral("reviewAssets"), reviewAssetCount);
    diagnostic::event(QStringLiteral("studio"),
                      QStringLiteral("generation-memory-before-release"), before);

    currentReviewItem_ = nullptr;
    currentMaterialItem_ = nullptr;
    if (reviewTree_)
        reviewTree_->clear();
    setReviewOptions({});
    clearLayout(reviewVisualLayout_);
    reviewQuestionEditorPanel_->setVisible(false);
    reviewVisualPanel_->setVisible(false);

    generatedMaterials_ = {};
    generatedQuestions_ = {};
    reviewQuestions_ = {};
    generatedAssets_.clear();
    generatedAssets_.squeeze();
    reviewSourceImages_.clear();
    reviewSourceImages_.squeeze();
    lazyReviewAssets_.clear();
    reviewPdfCache_.clear();
    reviewAssets_.clear();
    reviewAssets_.squeeze();
    pendingCropAsset_ = QJsonObject{};
    pendingCropPage_ = QImage{};
    generatedHasAnswerKey_ = true;

    QVariantMap after = diagnostic::memorySnapshot();
    after.insert(QStringLiteral("releasedAssetBytes"),
                 generatedAssetBytes + reviewAssetBytes);
    after.insert(QStringLiteral("releasedAssets"),
                 generatedAssetCount + reviewAssetCount);
    diagnostic::event(QStringLiteral("studio"),
                      QStringLiteral("generation-memory-after-release"), after);
}

} // namespace quizpane::studio
