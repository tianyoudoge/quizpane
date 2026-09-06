#include "solution_page_controller.hpp"

#include "line_icons.hpp"
#include "material_card.hpp"
#include "quizpane/pending_call.hpp"
#include "quizpane/provider_loader.hpp"
#include "quizpane/provider_response_router.hpp"
#include "result_image_preview.hpp"

#include <QColor>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStringList>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>

namespace quizpane {
namespace {

using ui::LineIcon;
using ui::makeLineIcon;

QString choiceLabel(int choice) {
    return choice >= 0 ? QString(QChar(u'A' + choice)) : QStringLiteral("未作答");
}

}  // namespace

void SolutionPageController::init(QWidget* solutionPage, QStackedWidget* pages,
                                  QWidget* catalogPage, ProviderLoader& provider,
                                  AttemptSession& session, UiSize& uiSize,
                                  std::function<void()> onApplyUiSize) {
    solutionPage_ = solutionPage; pages_ = pages; catalogPage_ = catalogPage;
    provider_ = &provider; session_ = &session; uiSize_ = &uiSize;
    onApplyUiSize_ = std::move(onApplyUiSize);
}

void SolutionPageController::buildInto() {
    auto* solutionLayout = new QVBoxLayout(solutionPage_);
    solutionLayout->setContentsMargins(0, 14, 0, 0);
    solutionLayout->setSpacing(8);
    resultSummaryLabel_ = new QLabel(QStringLiteral("答题结果"));
    resultSummaryLabel_->setObjectName(QStringLiteral("pageTitle"));
    resultSummaryLabel_->setWordWrap(true);
    solutionProgressLabel_ = new QLabel;
    solutionProgressLabel_->setObjectName(QStringLiteral("detail"));
    auto* solutionScroll = new QScrollArea;
    solutionScroll->setWidgetResizable(true);
    solutionScroll->setFrameShape(QFrame::NoFrame);
    auto* solutionContent = new QWidget;
    solutionContentLayout_ = new QVBoxLayout(solutionContent);
    solutionContentLayout_->setContentsMargins(0, 4, 4, 4);
    solutionMaterialCard_ = new ui::MaterialCard;
    solutionQuestionLabel_ = new QLabel;
    solutionQuestionLabel_->setObjectName(QStringLiteral("solutionQuestion"));
    solutionQuestionLabel_->setWordWrap(true);
    solutionQuestionLabel_->setTextFormat(Qt::RichText);
    solutionAnswerLabel_ = new QLabel;
    solutionAnswerLabel_->setObjectName(QStringLiteral("resultAnswer"));
    auto* answerSummary = new QHBoxLayout(solutionAnswerLabel_);
    answerSummary->setContentsMargins(8, 7, 8, 7);
    answerSummary->setSpacing(8);
    selectedAnswerLabel_ = new QLabel;
    correctAnswerLabel_ = new QLabel;
    answerStatusLabel_ = new QLabel;
    answerStatusLabel_->setObjectName(QStringLiteral("answerStatus"));
    for (QLabel* label : {selectedAnswerLabel_, correctAnswerLabel_, answerStatusLabel_})
        label->setWordWrap(true);
    answerSummary->addWidget(selectedAnswerLabel_);
    answerSummary->addWidget(correctAnswerLabel_);
    answerSummary->addStretch();
    answerSummary->addWidget(answerStatusLabel_);
    solutionExplanationLabel_ = new QLabel;
    solutionExplanationLabel_->setObjectName(QStringLiteral("solutionText"));
    solutionExplanationLabel_->setWordWrap(true);
    solutionExplanationLabel_->setTextFormat(Qt::RichText);
    solutionContentLayout_->addWidget(solutionMaterialCard_);
    solutionContentLayout_->addWidget(solutionQuestionLabel_);
    solutionContentLayout_->addSpacing(8);
    solutionContentLayout_->addWidget(solutionAnswerLabel_);
    solutionContentLayout_->addWidget(solutionExplanationLabel_);
    solutionContentLayout_->addStretch();
    solutionScroll->setWidget(solutionContent);
    solutionControlBar_ = new QWidget;
    solutionControlBar_->setObjectName(QStringLiteral("controlBar"));
    auto* solutionNav = new QHBoxLayout(solutionControlBar_);
    solutionNav->setContentsMargins(0, 0, 0, 0);
    previousSolutionButton_ = new QPushButton;
    nextSolutionButton_ = new QPushButton;
    exportResultsButton_ = new QPushButton(QStringLiteral("查看作答结果"));
    exportResultsButton_->setObjectName(QStringLiteral("smallButton"));
    exportResultsButton_->setToolTip(QStringLiteral("预览适合手机查看的作答结果长图"));
    auto* backToCatalogButton = new QPushButton;
    previousSolutionButton_->setIcon(makeLineIcon(LineIcon::Previous));
    nextSolutionButton_->setIcon(makeLineIcon(LineIcon::Next));
    backToCatalogButton->setIcon(makeLineIcon(LineIcon::Catalog));
    for (auto* button : {previousSolutionButton_, nextSolutionButton_, backToCatalogButton})
        button->setObjectName(QStringLiteral("navIconButton"));
    previousSolutionButton_->setAccessibleName(QStringLiteral("上一题解析"));
    previousSolutionButton_->setToolTip(QStringLiteral("上一题"));
    nextSolutionButton_->setAccessibleName(QStringLiteral("下一题解析"));
    nextSolutionButton_->setToolTip(QStringLiteral("下一题"));
    backToCatalogButton->setAccessibleName(QStringLiteral("返回分类"));
    backToCatalogButton->setToolTip(QStringLiteral("返回分类"));
    solutionNav->addWidget(previousSolutionButton_);
    solutionNav->addWidget(nextSolutionButton_);
    solutionNav->addStretch();
    solutionNav->addWidget(exportResultsButton_);
    solutionNav->addWidget(backToCatalogButton);
    QObject::connect(previousSolutionButton_, &QPushButton::clicked, solutionPage_,
            [this] { showSolution(currentSolutionIndex_ - 1); });
    QObject::connect(nextSolutionButton_, &QPushButton::clicked, solutionPage_,
            [this] { showSolution(currentSolutionIndex_ + 1); });
    QObject::connect(exportResultsButton_, &QPushButton::clicked, solutionPage_,
            [this] { exportAttemptResults(); });
    QObject::connect(backToCatalogButton, &QPushButton::clicked, solutionPage_,
            [this] {
                pages_->setCurrentWidget(catalogPage_);
                if (onApplyUiSize_) onApplyUiSize_();
            });
    solutionLayout->addWidget(resultSummaryLabel_);
    solutionLayout->addWidget(solutionProgressLabel_);
    solutionLayout->addWidget(solutionScroll, 1);
    solutionLayout->addWidget(solutionControlBar_);
}

bool SolutionPageController::isCurrentPage() const {
    return pages_->currentWidget() == solutionPage_;
}

void SolutionPageController::setResultSummary(const QString& text) {
    resultSummaryLabel_->setText(text);
}

void SolutionPageController::setNoSolutionsMessage() {
    solutionProgressLabel_->setText(QStringLiteral("暂无题目解析"));
}

void SolutionPageController::requestResults() {
    resultSummaryLabel_->setText(QStringLiteral("正在生成答题结果…"));
    pages_->setCurrentWidget(solutionPage_);
    if (onApplyUiSize_) onApplyUiSize_();
    QString error;
    if (!provider_->request(
            makePendingCall(ProviderRoute::Report,
                            QJsonObject{{"attemptId", session_->attemptId}}).build(), &error)) {
        QMessageBox::warning(solutionPage_, QStringLiteral("无法生成答题结果"), error);
        return;
    }
    if (session_->attemptHasAnswerKey &&
        !provider_->request(
            makePendingCall(ProviderRoute::Solutions,
                            QJsonObject{{"attemptId", session_->attemptId}}).build(), &error)) {
        QMessageBox::warning(solutionPage_, QStringLiteral("无法加载题目解析"), error);
    }
}

void SolutionPageController::showSolution(int index) {
    if (session_->solutions.isEmpty()) return;
    currentSolutionIndex_ = qBound(0, index, static_cast<int>(session_->solutions.size()) - 1);
    const QJsonObject solution = session_->solutions.at(currentSolutionIndex_).toObject();
    const QString materialId = solution.value("materialId").toString();
    if (materialId.isEmpty()) {
        solutionMaterialCard_->hideMaterial();
    } else {
        const QJsonObject material = session_->materialsById.value(materialId);
        solutionMaterialCard_->showMaterial(materialId, material.value("title").toString(),
                                            material.value("contentHtml").toString(),
                                            material.value("imageUrls").toArray());
    }
    QString optionsHtml;
    for (const auto& optionValue : solution.value("options").toArray()) {
        const auto option = optionValue.toObject();
        optionsHtml += QStringLiteral("<div class=\"option\"><b>%1.</b> %2</div>")
            .arg(option.value("label").toString().toHtmlEscaped(),
                 option.value("contentHtml").toString());
    }
    solutionQuestionLabel_->setText(
        QStringLiteral("<div style=\"color:#c7ccd2\">%1%2</div>")
            .arg(solution.value("contentHtml").toString(), optionsHtml));
    const bool multiple = solution.value("type").toString() == QStringLiteral("multiple_choice");
    const QSet<int> selectedChoices = session_->answers.value(currentSolutionIndex_);
    const int selected = selectedChoices.isEmpty() ? -1 : *selectedChoices.constBegin();
    const auto labels = [](const QSet<int>& set) { QStringList result; for (int value : set) result.append(choiceLabel(value)); std::sort(result.begin(), result.end()); return result.join(QStringLiteral("、")); };
    selectedAnswerLabel_->setText(QStringLiteral("你的答案\n%1").arg(multiple ? labels(selectedChoices) : choiceLabel(selected)));
    if (session_->attemptHasAnswerKey) {
        const int correct = solution.value("correctChoice").toInt(-1);
        QSet<int> correctChoices;
        for (const auto& value : solution.value("correctChoices").toArray())
            correctChoices.insert(value.toInt());
        const bool isCorrect = selectedChoices == correctChoices;
        correctAnswerLabel_->setVisible(true);
        answerStatusLabel_->setVisible(true);
        solutionExplanationLabel_->setVisible(true);
        correctAnswerLabel_->setText(QStringLiteral("正确答案\n%1").arg(
            multiple ? labels(correctChoices) : choiceLabel(correct)));
        answerStatusLabel_->setText(isCorrect ? QStringLiteral("✓ 正确") : QStringLiteral("✗ 错误"));
        answerStatusLabel_->setProperty("correct", isCorrect);
        answerStatusLabel_->style()->unpolish(answerStatusLabel_);
        answerStatusLabel_->style()->polish(answerStatusLabel_);
        solutionExplanationLabel_->setText(
            QStringLiteral("<div style=\"color:#aebbb5\"><p><b>解析</b></p>%1</div>")
                .arg(solution.value("solutionHtml").toString()));
    } else {
        correctAnswerLabel_->setVisible(false);
        answerStatusLabel_->setVisible(false);
        solutionExplanationLabel_->setVisible(false);
    }
    solutionProgressLabel_->setText(QStringLiteral("第 %1 / %2 题")
        .arg(currentSolutionIndex_ + 1).arg(session_->solutions.size()));
    previousSolutionButton_->setEnabled(currentSolutionIndex_ > 0);
    nextSolutionButton_->setEnabled(currentSolutionIndex_ + 1 < session_->solutions.size());
}

void SolutionPageController::exportAttemptResults() {
    const auto exportQuestions = session_->questions;
    const auto exportAnswers = session_->answers;
    const auto exportTitle = session_->attemptTitle;
    const auto exportHasAnswerKey = session_->attemptHasAnswerKey;
    if (exportQuestions.isEmpty())
        return;
    const int imageWidth = ui::resultImagePixelWidth();
    constexpr int padding = 32;
    constexpr int columns = 2;
    constexpr int maxRowsPerImage = 72;
    constexpr int headerHeight = 190;
    constexpr int gap = 12;

    const auto answerText = [&](qsizetype index, const QJsonObject& question) {
        const QSet<int> choices = exportAnswers.value(index);
        if (question.value("type").toString() == QStringLiteral("multiple_choice")) {
            QStringList labels;
            for (const int choice : choices) labels.append(choiceLabel(choice));
            std::sort(labels.begin(), labels.end());
            return labels.isEmpty() ? QStringLiteral("未作答") : labels.join(QStringLiteral("、"));
        }
        return choiceLabel(choices.isEmpty() ? -1 : *choices.constBegin());
    };
    // 无答案题库只承担"记录选择"的职责。五题起按每五题一组汇总，避免
    // 大题库生成数百张宽卡片；多选用方括号保留边界，未作答用 ? 标记。
    const bool compactMode = !exportHasAnswerKey && exportQuestions.size() >= 5;
    QStringList compactTokens;
    if (compactMode) {
        compactTokens.reserve(exportQuestions.size());
        for (int index = 0; index < exportQuestions.size(); ++index) {
            QString token = answerText(index, exportQuestions.at(index).toObject());
            if (token == QStringLiteral("未作答")) {
                token = QStringLiteral("?");
            } else if (token.contains(QStringLiteral("、"))) {
                token.remove(QStringLiteral("、"));
                token = QStringLiteral("[%1]").arg(token);
            }
            compactTokens.append(token);
        }
    }
    const QList<ui::CompactResultRow> compactRows = ui::compactResultRows(compactTokens);
    const int displayItemCount = compactMode ? compactRows.size() : int(exportQuestions.size());
    const int rowHeight = compactMode ? 66 : 74;
    const int itemCapacity = columns * maxRowsPerImage;
    const int pageCount = (displayItemCount + itemCapacity - 1) / itemCapacity;
    if (pageCount == 0) return;
    int answered = 0;
    for (int index = 0; index < exportQuestions.size(); ++index) {
        if (!exportAnswers.value(index).isEmpty())
            ++answered;
    }
    const QString exportedAt = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    const auto renderPage = [&](int page) -> QImage {
        const int first = page * itemCapacity;
        const int count = qMin(itemCapacity, displayItemCount - first);
        const int rows = (count + columns - 1) / columns;
        QImage image(imageWidth, headerHeight + rows * rowHeight + padding,
                     QImage::Format_ARGB32_Premultiplied);
        if (image.isNull()) return {};
        image.fill(QColor(QStringLiteral("#10151c")));
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        QFont titleFont = painter.font();
        titleFont.setPixelSize(34);
        titleFont.setBold(true);
        painter.setFont(titleFont);
        painter.setPen(QColor(QStringLiteral("#f2f5f8")));
        painter.drawText(padding, 66, QStringLiteral("作答结果"));
        QFont detailFont = painter.font();
        detailFont.setPixelSize(19);
        detailFont.setBold(false);
        painter.setFont(detailFont);
        painter.setPen(QColor(QStringLiteral("#aeb8c3")));
        painter.drawText(padding, 104, exportTitle.isEmpty() ? QStringLiteral("题库练习") : exportTitle);
        painter.drawText(padding, 140, QStringLiteral("已作答 %1 / %2 题 · %3")
            .arg(answered).arg(exportQuestions.size())
            .arg(exportedAt));
        painter.drawText(padding, 174, exportHasAnswerKey
            ? QStringLiteral("仅汇总你的选择，便于快速对照答案")
            : QStringLiteral("无答案题库：仅记录你的选择，不提供判分"));
        if (pageCount > 1)
            painter.drawText(imageWidth - padding - 90, 174,
                             QStringLiteral("%1 / %2").arg(page + 1).arg(pageCount));
        const qreal cardWidth = (imageWidth - padding * 2 - gap * (columns - 1)) / qreal(columns);
        for (int offset = 0; offset < count; ++offset) {
            const int index = first + offset;
            const int column = offset % columns;
            const int row = offset / columns;
            const QRectF card(padding + column * (cardWidth + gap), headerHeight + row * rowHeight,
                              cardWidth, rowHeight - gap);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(QStringLiteral("#1b232d")));
            painter.drawRoundedRect(card, 12, 12);
            if (compactMode) {
                const ui::CompactResultRow& compactRow = compactRows.at(index);
                QFont rangeFont = painter.font();
                rangeFont.setPixelSize(18);
                rangeFont.setBold(true);
                painter.setFont(rangeFont);
                painter.setPen(QColor(QStringLiteral("#e7edf4")));
                painter.drawText(card.adjusted(16, 8, -16, -8),
                                 Qt::AlignLeft | Qt::AlignVCenter, compactRow.rangeLabel);

                QFont sequenceFont = painter.font();
                sequenceFont.setPixelSize(22);
                sequenceFont.setBold(true);
                painter.setFont(sequenceFont);
                const int sequenceWidth = qMax(54, int(card.width())
                    - 44 - painter.fontMetrics().horizontalAdvance(compactRow.rangeLabel));
                while (sequenceFont.pixelSize() > 15
                       && painter.fontMetrics().horizontalAdvance(compactRow.answerSequence) > sequenceWidth) {
                    sequenceFont.setPixelSize(sequenceFont.pixelSize() - 1);
                    painter.setFont(sequenceFont);
                }
                painter.setPen(compactRow.answerSequence.contains(QLatin1Char('?'))
                    ? QColor(QStringLiteral("#c99db5")) : QColor(QStringLiteral("#f45aa6")));
                painter.drawText(card.adjusted(16, 8, -16, -8),
                                 Qt::AlignRight | Qt::AlignVCenter, compactRow.answerSequence);
                continue;
            }

            const QJsonObject question = exportQuestions.at(index).toObject();
            const int sourceNumber = question.value("sourceQuestionNumber").toInt();
            const QString sectionTitle = question.value("sourceSectionTitle").toString();
            QString sourceLabel = question.value("sourceQuestionLabel").toString();
            if (sourceLabel.isEmpty()) sourceLabel = sourceNumber > 0
                ? QString::number(sourceNumber) : QString::number(index + 1);
            // 长标签若省略尾部会丢掉"第几处"。重号使用本次
            // 练习的独立序号 + 原题号，两者都保留，不按原题号覆盖或合并。
            const bool repeatedLabel = sourceNumber > 0 && sourceLabel != QString::number(sourceNumber);
            const QString exportLabel = repeatedLabel
                ? QStringLiteral("%1 · 原%2").arg(index + 1).arg(sourceNumber)
                : (sectionTitle.isEmpty() ? sourceLabel : QStringLiteral("%1 · %2").arg(sectionTitle, sourceLabel));
            QFont numberFont = painter.font();
            numberFont.setPixelSize(sectionTitle.isEmpty() && sourceLabel == QString::number(sourceNumber) ? 22 : 15);
            numberFont.setBold(true);
            painter.setFont(numberFont);
            painter.setPen(QColor(QStringLiteral("#e7edf4")));
            painter.drawText(card.adjusted(16, 8, -16, -8), Qt::AlignLeft | Qt::AlignTop,
                             painter.fontMetrics().elidedText(exportLabel,
                                 Qt::ElideRight, qMax(30, int(card.width()) - 100)));
            QFont choiceFont = painter.font();
            choiceFont.setPixelSize(24);
            choiceFont.setBold(true);
            painter.setFont(choiceFont);
            const QString choice = answerText(index, question);
            painter.setPen(choice == QStringLiteral("未作答") ? QColor(QStringLiteral("#8995a3"))
                                                               : QColor(QStringLiteral("#f45aa6")));
            painter.drawText(card.adjusted(16, 8, -16, -8), Qt::AlignRight | Qt::AlignVCenter, choice);
        }
        painter.end();
        return image;
    };

    QDialog dialog(solutionPage_);
    dialog.setWindowTitle(QStringLiteral("作答结果"));
    dialog.resize(760, 700);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(16, 14, 16, 16);
    layout->setSpacing(10);
    auto* toolbar = new QHBoxLayout;
    auto* title = new QLabel(QStringLiteral("作答结果长图"));
    title->setObjectName(QStringLiteral("sectionTitle"));
    auto* previous = new QPushButton;
    auto* next = new QPushButton;
    previous->setObjectName(QStringLiteral("navIconButton"));
    next->setObjectName(QStringLiteral("navIconButton"));
    previous->setIcon(makeLineIcon(LineIcon::Previous));
    next->setIcon(makeLineIcon(LineIcon::Next));
    previous->setToolTip(QStringLiteral("上一张"));
    next->setToolTip(QStringLiteral("下一张"));
    auto* pageLabel = new QLabel;
    pageLabel->setObjectName(QStringLiteral("detail"));
    auto* save = new QPushButton;
    save->setObjectName(QStringLiteral("navIconButton"));
    save->setIcon(makeLineIcon(LineIcon::Save));
    save->setToolTip(QStringLiteral("保存当前图片"));
    save->setAccessibleName(QStringLiteral("保存当前图片"));
    toolbar->addWidget(title);
    toolbar->addStretch();
    toolbar->addWidget(previous);
    toolbar->addWidget(pageLabel);
    toolbar->addWidget(next);
    toolbar->addSpacing(6);
    toolbar->addWidget(save);
    layout->addLayout(toolbar);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(false);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* preview = new QLabel;
    preview->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    scroll->setWidget(preview);
    layout->addWidget(scroll, 1);
    int currentImage = 0;
    QImage displayedImage;
    const auto showImage = [&] {
        // Release the previous image/pixmap before allocating the next page.
        displayedImage = {};
        preview->clear();
        displayedImage = renderPage(currentImage);
        if (displayedImage.isNull())
            preview->setText(QStringLiteral("内存不足，无法生成当前图片，请减少练习题量后重试。"));
        else
            ui::setResultPreviewImage(preview, displayedImage);
        save->setEnabled(!displayedImage.isNull());
        pageLabel->setText(QStringLiteral("%1 / %2").arg(currentImage + 1).arg(pageCount));
        previous->setEnabled(currentImage > 0);
        next->setEnabled(currentImage + 1 < pageCount);
    };
    QObject::connect(previous, &QPushButton::clicked, &dialog, [&] {
        if (currentImage > 0) { --currentImage; showImage(); }
    });
    QObject::connect(next, &QPushButton::clicked, &dialog, [&] {
        if (currentImage + 1 < pageCount) { ++currentImage; showImage(); }
    });
    QObject::connect(save, &QPushButton::clicked, &dialog, [&] {
        const QString suffix = pageCount > 1
            ? QStringLiteral("-%1").arg(currentImage + 1) : QString();
        const QString suggested = QDir(QStandardPaths::writableLocation(
            QStandardPaths::PicturesLocation)).filePath(QStringLiteral("作答结果%1.png").arg(suffix));
        const QString path = QFileDialog::getSaveFileName(&dialog, QStringLiteral("保存作答结果"),
            suggested, QStringLiteral("PNG 图片 (*.png)"));
        if (!path.isEmpty() && !displayedImage.save(path, "PNG"))
            QMessageBox::warning(&dialog, QStringLiteral("保存失败"),
                                 QStringLiteral("无法写入图片：%1").arg(path));
    });
    showImage();
    dialog.exec();
}

}  // namespace quizpane
