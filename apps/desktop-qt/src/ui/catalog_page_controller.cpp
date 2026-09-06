#include "catalog_page_controller.hpp"

#include "quizpane/pending_call.hpp"
#include "quizpane/provider_loader.hpp"
#include "quizpane/provider_response_router.hpp"

#include <QCheckBox>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace quizpane {

void CatalogPageController::init(QWidget* catalogPage, QStackedWidget* pages,
                                 ProviderLoader& provider, QWidget* loginPage,
                                 QLabel* loginTitleLabel, QLabel* loginDetailLabel,
                                 std::function<void()> onApplyUiSize) {
    catalogPage_ = catalogPage; pages_ = pages; provider_ = &provider; loginPage_ = loginPage;
    loginTitleLabel_ = loginTitleLabel; loginDetailLabel_ = loginDetailLabel;
    onApplyUiSize_ = std::move(onApplyUiSize);
}

void CatalogPageController::buildInto() {
    auto* catalogPageLayout = new QVBoxLayout(catalogPage_);
    catalogPageLayout->setContentsMargins(0, 14, 0, 0);
    auto* catalogTitle = new QLabel(QStringLiteral("选择练习"));
    catalogTitle->setObjectName(QStringLiteral("pageTitle"));
    auto* catalogHint = new QLabel(QStringLiteral("选择分类和题量，立即开始一组练习。"));
    catalogHint->setObjectName(QStringLiteral("detail"));
    auto* catalogScroll = new QScrollArea;
    catalogScroll->setWidgetResizable(true);
    catalogScroll->setFrameShape(QFrame::NoFrame);
    auto* catalogContent = new QWidget;
    catalogListLayout_ = new QVBoxLayout(catalogContent);
    catalogListLayout_->setContentsMargins(0, 4, 4, 4);
    catalogListLayout_->setSpacing(10);
    catalogListLayout_->addStretch();
    catalogScroll->setWidget(catalogContent);
    catalogPageLayout->addWidget(catalogTitle);
    catalogPageLayout->addWidget(catalogHint);
    catalogPageLayout->addWidget(catalogScroll, 1);
}

void CatalogPageController::requestCatalog() {
    pages_->setCurrentWidget(loginPage_);
    if (onApplyUiSize_) onApplyUiSize_();
    loginTitleLabel_->setText(QStringLiteral("正在加载练习分类…"));
    QString error;
    if (!provider_->request(makePendingCall(ProviderRoute::Catalog).build(), &error))
        loginDetailLabel_->setText(error);
}

namespace {
void clearLayout(QLayout* layout) {
    while (layout && layout->count() > 0) {
        QLayoutItem* item = layout->takeAt(0);
        if (item->layout()) clearLayout(item->layout());
        delete item->widget();
        delete item;
    }
}
}  // namespace

void CatalogPageController::populateCatalog(
    const QJsonArray& nodes,
    const std::function<void(const QString&, const QString&, int, bool)>& onStartAttempt) {
    clearLayout(catalogListLayout_);
    for (const auto& value : nodes) {
        if (!value.isObject()) continue;
        const QJsonObject node = value.toObject();
        const QString categoryId = node.value("id").toString();
        const QString title = node.value("title").toString();
        const int available = node.value("availableQuestionCount").toInt();
        if (categoryId.isEmpty() || title.isEmpty()) continue;

        auto* row = new QWidget;
        row->setObjectName(QStringLiteral("catalogRow"));
        auto* rowLayout = new QVBoxLayout(row);
        rowLayout->setContentsMargins(12, 10, 10, 10);
        rowLayout->setSpacing(7);
        auto* label = new QLabel(QStringLiteral("%1\n%2 道可练").arg(title).arg(available));
        const int mastered = node.value("masteredCount").toInt();
        const int mistakes = node.value("mistakeCount").toInt();
        label->setText(QStringLiteral("%1\n%2 道可练 · 已掌握 %3 · 待巩固 %4")
            .arg(title).arg(available).arg(mastered).arg(mistakes));
        label->setWordWrap(true);
        rowLayout->addWidget(label);
        auto* includeAnswered = new QCheckBox(QStringLiteral("包含之前做过的题"));
        includeAnswered->setObjectName(QStringLiteral("includeAnswered"));
        includeAnswered->setChecked(false);
        rowLayout->addWidget(includeAnswered);
        QList<int> counts;
        for (const auto& countValue : node.value("suggestedCounts").toArray()) {
            const int count = countValue.toInt();
            if (count > 0 && count <= available && !counts.contains(count)) counts.append(count);
        }
        for (const int preset : {5, 10, 15, 20})
            if (preset <= available && !counts.contains(preset)) counts.append(preset);
        if (available > 0 && !counts.contains(available)) counts.append(available);
        std::sort(counts.begin(), counts.end());
        auto* countLayout = new QGridLayout;
        countLayout->setContentsMargins(0, 0, 0, 0);
        countLayout->setHorizontalSpacing(6);
        countLayout->setVerticalSpacing(6);
        int buttonIndex = 0;
        for (const int count : counts) {
            auto* button = new QPushButton(QStringLiteral("%1 题").arg(count));
            if (count == available) button->setText(QStringLiteral("全部 %1 题").arg(available));
            button->setObjectName(QStringLiteral("smallButton"));
            QObject::connect(button, &QPushButton::clicked, catalogPage_,
                    [onStartAttempt, categoryId, title, count, includeAnswered] {
                        onStartAttempt(categoryId, title, count, includeAnswered->isChecked());
                    });
            countLayout->addWidget(button, buttonIndex / 3, buttonIndex % 3);
            ++buttonIndex;
        }
        rowLayout->addLayout(countLayout);
        catalogListLayout_->addWidget(row);
    }
    catalogListLayout_->addStretch();
    pages_->setCurrentWidget(catalogPage_);
    if (onApplyUiSize_) onApplyUiSize_();
}

void CatalogPageController::returnToCatalog() {
    pages_->setCurrentWidget(catalogPage_);
    if (onApplyUiSize_) onApplyUiSize_();
}

}  // namespace quizpane
