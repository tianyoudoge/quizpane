#include "solution_page_controller.hpp"

#include "../ai/ai_explain_service.hpp"
#include "../ai/ai_explanation_store.hpp"
#include "../app_settings.hpp"
#include "line_icons.hpp"
#include "material_card.hpp"
#include "formula_formatter.hpp"
#include "quizpane/pending_call.hpp"
#include "quizpane/provider_loader.hpp"
#include "quizpane/provider_response_router.hpp"
#include "result_image_preview.hpp"

#include <QColor>
#include <QComboBox>
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
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSet>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStringList>
#include <QStyle>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace quizpane {
namespace {

using ui::LineIcon;
using ui::makeLineIcon;

QString choiceLabel(int choice) {
    return choice >= 0 ? QString(QChar(u'A' + choice)) : QStringLiteral("未作答");
}

QString inlineOptionHtml(const QString& html) {
    static const QRegularExpression paragraph(
        QStringLiteral("^\\s*<p(?:\\s[^>]*)?>([\\s\\S]*)</p>\\s*$"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression nestedParagraph(
        QStringLiteral("<\\s*/?\\s*p\\b"), QRegularExpression::CaseInsensitiveOption);
    const auto match = paragraph.match(html);
    if (!match.hasMatch() || match.captured(1).contains(nestedParagraph)) return html;
    return match.captured(1);
}

bool hasVisibleExplanation(const QString& html) {
    if (html.contains(QRegularExpression(QStringLiteral("<\\s*img\\b"),
                                          QRegularExpression::CaseInsensitiveOption))) return true;
    QTextDocument document;
    document.setHtml(html);
    return !document.toPlainText().trimmed().isEmpty();
}

}  // namespace

QString formatAiResponse(const QString& raw) {
    QString result = normaliseFormulas(raw);

    // Style section headers 【答案确认】 etc.
    static const QRegularExpression headerRe(
        QStringLiteral("【(答案确认|解题步骤|知识点|速算技巧)】"));
    result.replace(headerRe,
        QStringLiteral("<span style=\"color:#9fc4b0;font-weight:600;"
                       "border-left:3px solid #5a9a80;padding-left:6px;display:inline-block;"
                       "margin-top:6px;\">【\\1】</span>"));

    // Highlight correct answer letter(s)
    static const QRegularExpression correctRe(
        QStringLiteral("正确答案[：:]?\\s*([A-D]+)"));
    result.replace(correctRe,
        QStringLiteral("正确答案：<span style=\"color:#9fb6a7;font-weight:700;"
                       "text-decoration:underline;\">\\1</span>"));

    // Speed-math / tips section: italicise and tint amber
    static const QRegularExpression tipsContentRe(
        QStringLiteral("(【速算技巧】.*?)(?=【|$)"),
        QRegularExpression::DotMatchesEverythingOption);
    result.replace(tipsContentRe,
        QStringLiteral("<span style=\"color:#c8a96e;font-style:italic;\">\\1</span>"));

    return result;
}


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
    aiExplainPanel_ = new QFrame;
    aiExplainPanel_->setObjectName(QStringLiteral("aiExplainPanel"));
    aiExplainPanel_->setVisible(false);
    auto* aiPanelLayout = new QVBoxLayout(aiExplainPanel_);
    aiPanelLayout->setContentsMargins(10, 10, 10, 10);
    aiPanelLayout->setSpacing(6);
    auto* aiPanelTitle = new QLabel(QStringLiteral("✦ AI 解析"));
    aiPanelTitle->setObjectName(QStringLiteral("aiExplainTitle"));
    aiExplainContentLabel_ = new QLabel;
    aiExplainContentLabel_->setObjectName(QStringLiteral("aiExplainContent"));
    aiExplainContentLabel_->setWordWrap(true);
    aiExplainContentLabel_->setTextFormat(Qt::RichText);
    aiExplainContentLabel_->setOpenExternalLinks(false);
    aiExplainContentLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    aiSaveStatusLabel_ = new QLabel;
    aiSaveStatusLabel_->setObjectName(QStringLiteral("detail"));
    aiSaveStatusLabel_->setWordWrap(true);
    aiPanelLayout->addWidget(aiPanelTitle);
    aiPanelLayout->addWidget(aiExplainContentLabel_);
    aiPanelLayout->addWidget(aiSaveStatusLabel_);
    solutionContentLayout_->addWidget(aiExplainPanel_);
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
    aiExplainButton_ = new QPushButton(QStringLiteral("✦ AI 解析"));
    aiExplainButton_->setObjectName(QStringLiteral("smallButton"));
    aiExplainButton_->setToolTip(QStringLiteral("用 AI 解析这道题的解题步骤和知识点"));
    aiExplainButton_->setMinimumWidth(80);
    aiConfigButton_ = new QPushButton(QStringLiteral("⚙"));
    aiConfigButton_->setObjectName(QStringLiteral("aiConfigButton"));
    aiConfigButton_->setAccessibleName(QStringLiteral("AI 解析设置"));
    aiConfigButton_->setToolTip(QStringLiteral("AI 解析设置"));
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
    solutionNav->addWidget(aiExplainButton_);
    solutionNav->addWidget(aiConfigButton_);
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
    aiService_ = new AiExplainService(solutionPage_);
    aiSpinnerTimer_ = new QTimer(solutionPage_);
    aiSpinnerTimer_->setInterval(120);
    QObject::connect(aiSpinnerTimer_, &QTimer::timeout, solutionPage_,
        [this] {
            static const char* frames[] = {
                "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"
            };
            aiSpinnerFrame_ = (aiSpinnerFrame_ + 1) % 10;
            aiExplainButton_->setText(
                QStringLiteral("%1 解析中").arg(QString::fromUtf8(frames[aiSpinnerFrame_])));
        });
    QObject::connect(aiExplainButton_, &QPushButton::clicked, solutionPage_,
        [this] { onAiExplainClicked(); });
    QObject::connect(aiConfigButton_, &QPushButton::clicked, solutionPage_,
        [this] { showAiConfigDialog(); });
    QObject::connect(aiService_, &AiExplainService::requestStarted, solutionPage_,
        [this] {
            aiExplainButton_->setEnabled(false);
            aiSpinnerFrame_ = 0;
            aiSpinnerTimer_->start();
            aiExplainContentLabel_->setText(QStringLiteral("正在请求 AI 解析，请稍候…"));
            aiSaveStatusLabel_->clear();
            aiExplainPanel_->setVisible(true);
        });
    QObject::connect(aiService_, &AiExplainService::responseReady, solutionPage_,
        [this](const QString& html, quizpane::AiUsage usage) {
            aiSpinnerTimer_->stop();
            aiExplainButton_->setEnabled(true);
            aiExplainButton_->setText(QStringLiteral("✦ AI 解析"));
            AppSettings::incrementAiTotalRequests();
            AppSettings::addAiPromptTokens(usage.promptTokens);
            AppSettings::addAiCompletionTokens(usage.completionTokens);
            aiExplainContentLabel_->setText(formatAiResponse(html));
            const QString id = session_->solutions.at(currentSolutionIndex_).toObject()
                                   .value(QStringLiteral("id")).toString();
            if (!id.isEmpty()) aiExplanations_.insert(id, html);
            QString saveError;
            const bool saved = saveAiExplanation(provider_->loadedPath(), id, html, &saveError);
            if (saved) unsavedAiIds_.remove(id);
            else unsavedAiIds_.insert(id);
            aiSaveStatusLabel_->setText(saved ? QStringLiteral("已保存到题库，可再次打开查看")
                                               : QStringLiteral("本次可回看，未写入题库：%1").arg(saveError));
            aiExplainButton_->setText(QStringLiteral("重新生成"));
            aiExplainPanel_->setVisible(true);
        });
    QObject::connect(aiService_, &AiExplainService::requestFailed, solutionPage_,
        [this](const QString& error) {
            aiSpinnerTimer_->stop();
            aiExplainButton_->setEnabled(true);
            aiExplainButton_->setText(QStringLiteral("✦ AI 解析"));
            aiExplainContentLabel_->setText(
                QStringLiteral("<span style=\"color:#c49f9d\">请求失败：%1</span>").arg(error.toHtmlEscaped()));
            aiExplainPanel_->setVisible(true);
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

void SolutionPageController::resetAiExplanations() {
    if (aiService_) aiService_->cancel();
    aiBankPath_.clear();
    aiExplanations_.clear();
    unsavedAiIds_.clear();
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
    const QString bankPath = provider_->loadedPath();
    if (bankPath != aiBankPath_) {
        aiBankPath_ = bankPath;
        aiExplanations_ = loadAiExplanations(bankPath);
    }
    if (aiExplainPanel_) {
        aiService_->cancel();
        aiSpinnerTimer_->stop();
        aiExplainPanel_->setVisible(false);
        aiExplainButton_->setEnabled(true);
        aiExplainButton_->setText(QStringLiteral("✦ AI 解析"));
    }
    currentSolutionIndex_ = qBound(0, index, static_cast<int>(session_->solutions.size()) - 1);
    const QJsonObject solution = session_->solutions.at(currentSolutionIndex_).toObject();
    const QString questionId = solution.value(QStringLiteral("id")).toString();
    const QString savedAi = aiExplanations_.value(questionId,
        solution.value(QStringLiteral("aiSolutionHtml")).toString());
    if (!savedAi.isEmpty()) {
        aiExplainContentLabel_->setText(formatAiResponse(savedAi));
        aiSaveStatusLabel_->setText(unsavedAiIds_.contains(questionId)
            ? QStringLiteral("仅本次会话可回看，未写入题库")
            : QStringLiteral("已保存到题库，可再次打开查看"));
        aiExplainPanel_->setVisible(true);
        aiExplainButton_->setText(QStringLiteral("重新生成"));
    }
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
                 formatQuestionHtml(inlineOptionHtml(option.value("contentHtml").toString())));
        const QString imageUrl = option.value(QStringLiteral("imageUrl")).toString();
        if (!imageUrl.isEmpty())
            optionsHtml += QStringLiteral("<p><img src=\"%1\" width=\"260\"></p>")
                               .arg(imageUrl.toHtmlEscaped());
    }
    solutionQuestionLabel_->setText(
        QStringLiteral("<div style=\"color:#c7ccd2\">%1%2</div>")
            .arg(formatQuestionHtml(solution.value("contentHtml").toString()), optionsHtml));
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
        // Native single-choice providers may only return correctChoice.
        // Empty selections never count as correct, even if an answer is missing.
        const bool isCorrect = !selectedChoices.isEmpty() && (multiple
            ? !correctChoices.isEmpty() && selectedChoices == correctChoices
            : correct >= 0 && selectedChoices == QSet<int>{correct});
        correctAnswerLabel_->setVisible(true);
        answerStatusLabel_->setVisible(true);
        const QString explanationHtml = solution.value("solutionHtml").toString();
        solutionExplanationLabel_->setVisible(hasVisibleExplanation(explanationHtml));
        correctAnswerLabel_->setText(QStringLiteral("正确答案\n%1").arg(
            multiple ? labels(correctChoices) : choiceLabel(correct)));
        answerStatusLabel_->setText(isCorrect ? QStringLiteral("✓ 正确") : QStringLiteral("✗ 错误"));
        answerStatusLabel_->setProperty("correct", isCorrect);
        answerStatusLabel_->style()->unpolish(answerStatusLabel_);
        answerStatusLabel_->style()->polish(answerStatusLabel_);
        solutionExplanationLabel_->setText(
            QStringLiteral("<div style=\"color:#aebbb5\"><p><b>解析</b></p>%1</div>")
                .arg(formatQuestionHtml(explanationHtml)));
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

void SolutionPageController::onAiExplainClicked() {
    QString apiKey = AppSettings::aiApiKey();
    if (apiKey.trimmed().isEmpty()) {
        if (!showAiConfigDialog()) return;
        apiKey = AppSettings::aiApiKey();
    }

    if (session_->solutions.isEmpty()) return;
    const QJsonObject solution = session_->solutions.at(currentSolutionIndex_).toObject();
    const QString questionHtml = solution.value(QStringLiteral("contentHtml")).toString();
    const QJsonArray options = solution.value(QStringLiteral("options")).toArray();
    const QJsonObject material = session_->materialsById
        .value(solution.value(QStringLiteral("materialId")).toString());
    const QString materialHtml = material.value(QStringLiteral("title")).toString() +
                                 QStringLiteral("\n") +
                                 material.value(QStringLiteral("contentHtml")).toString();
    const QJsonArray materialImageUrls = material.value(QStringLiteral("imageUrls")).toArray();
    QString correctAnswer;
    if (session_->attemptHasAnswerKey) {
        const QJsonArray correctChoices = solution.value(QStringLiteral("correctChoices")).toArray();
        if (!correctChoices.isEmpty()) {
            QStringList labels;
            for (const QJsonValue& value : correctChoices)
                labels.append(choiceLabel(value.toInt(-1)));
            correctAnswer = labels.join(QStringLiteral("、"));
        } else {
            const int choice = solution.value(QStringLiteral("correctChoice")).toInt(-1);
            if (choice >= 0) correctAnswer = choiceLabel(choice);
        }
    }
    aiService_->explain(questionHtml, options, materialHtml, materialImageUrls,
                        correctAnswer, apiKey,
                        AppSettings::aiBaseUrl(), AppSettings::aiModel());
}

bool SolutionPageController::showAiConfigDialog() {
    QDialog dialog(solutionPage_);
    dialog.setWindowTitle(QStringLiteral("AI 解析 · 配置"));
    dialog.setMinimumWidth(400);
    dialog.setStyleSheet(QStringLiteral(R"QSS(
        QDialog { background: #13181f; color: #c8cdd3; }
        QLabel { color: #c8cdd3; background: transparent; }
        QLabel#sectionTitle { color: #e0e3e7; font-weight: 600; }
        QLabel#detail { color: #9ca3ab; font-size: 11px; }
        QLineEdit, QComboBox {
            background: rgba(255,255,255,10);
            border: 1px solid rgba(255,255,255,14);
            border-radius: 7px;
            color: #c8cdd3;
            padding: 5px 8px;
        }
        QLineEdit:focus, QComboBox:focus { border-color: rgba(130,180,155,100); }
        QComboBox::drop-down { border: none; }
        QComboBox QAbstractItemView {
            background: #1b232d; color: #c8cdd3;
            selection-background-color: rgba(255,255,255,15);
            border: 1px solid rgba(255,255,255,18);
        }
        QPushButton {
            background: rgba(255,255,255,12); color: #c8cdd3;
            border: 1px solid rgba(255,255,255,14);
            border-radius: 7px; padding: 5px 12px;
        }
        QPushButton:hover { background: rgba(255,255,255,22); }
        QPushButton:disabled { background: transparent; color: rgba(170,176,184,70); }
        QPushButton#primaryButton {
            background: rgba(90,154,128,60);
            border-color: rgba(130,180,155,80); color: #d4ede5;
        }
        QPushButton#primaryButton:hover { background: rgba(90,154,128,90); }
        QFrame#divider { background: rgba(255,255,255,10); }
        QFrame#statsBox {
            background: rgba(255,255,255,6);
            border: 1px solid rgba(255,255,255,10); border-radius: 7px;
        }
    )QSS"));

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(12);

    auto* titleLabel = new QLabel(QStringLiteral("AI 解析设置"));
    titleLabel->setObjectName(QStringLiteral("sectionTitle"));
    QFont titleFont = titleLabel->font();
    titleFont.setPixelSize(14);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    layout->addWidget(titleLabel);

    layout->addWidget(new QLabel(QStringLiteral("供应商")));

    struct Provider { QString id, name, baseUrl, defaultModel; };
    const QList<Provider> providers = {
        {QStringLiteral("deepseek"), QStringLiteral("DeepSeek"),
         QStringLiteral("https://api.deepseek.com/v1"), QStringLiteral("deepseek-chat")},
        {QStringLiteral("aliyun"), QStringLiteral("阿里百炼"),
         QStringLiteral("https://dashscope.aliyuncs.com/compatible-mode/v1"), QStringLiteral("qwen-plus")},
        {QStringLiteral("volcengine"), QStringLiteral("火山方舟"),
         QStringLiteral("https://ark.cn-beijing.volces.com/api/v3"), QStringLiteral("doubao-pro-4k")},
        {QStringLiteral("zhipu"), QStringLiteral("智谱 GLM"),
         QStringLiteral("https://open.bigmodel.cn/api/paas/v4"), QStringLiteral("glm-4-flash")},
        {QStringLiteral("kimi"), QStringLiteral("Kimi"),
         QStringLiteral("https://api.moonshot.cn/v1"), QStringLiteral("moonshot-v1-8k")},
        {QStringLiteral("custom"), QStringLiteral("自定义"), QString(), QString()},
    };

    auto* providerCombo = new QComboBox;
    const QString savedProvider = AppSettings::aiProvider();
    int currentIdx = 0;
    for (int i = 0; i < providers.size(); ++i) {
        providerCombo->addItem(providers[i].name);
        if (providers[i].id == savedProvider) currentIdx = i;
    }
    providerCombo->setCurrentIndex(currentIdx);
    layout->addWidget(providerCombo);

    auto* regLinkLabel = new QLabel;
    regLinkLabel->setObjectName(QStringLiteral("detail"));
    regLinkLabel->setWordWrap(true);
    regLinkLabel->setOpenExternalLinks(true);
    regLinkLabel->setTextFormat(Qt::RichText);
    layout->addWidget(regLinkLabel);

    layout->addWidget(new QLabel(QStringLiteral("API Key")));
    auto* keyEdit = new QLineEdit;
    keyEdit->setPlaceholderText(QStringLiteral("sk-…"));
    keyEdit->setEchoMode(QLineEdit::Password);
    keyEdit->setText(AppSettings::aiApiKey());
    layout->addWidget(keyEdit);

    auto* advancedToggle = new QPushButton(QStringLiteral("▶ 高级设置"));
    advancedToggle->setFlat(true);
    advancedToggle->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: none; color: #9ca3ab; "
        "text-align: left; padding: 0; }"
        "QPushButton:hover { color: #c8cdd3; }"));
    layout->addWidget(advancedToggle);

    auto* advancedWidget = new QWidget;
    advancedWidget->setVisible(false);
    auto* advancedLayout = new QVBoxLayout(advancedWidget);
    advancedLayout->setContentsMargins(0, 0, 0, 0);
    advancedLayout->setSpacing(8);
    auto* baseUrlLabel = new QLabel(QStringLiteral("Base URL（留空使用默认）"));
    baseUrlLabel->setObjectName(QStringLiteral("detail"));
    auto* baseUrlEdit = new QLineEdit;
    baseUrlEdit->setPlaceholderText(QStringLiteral("https://api.example.com/v1"));
    baseUrlEdit->setText(AppSettings::aiBaseUrl());
    auto* modelLabel = new QLabel(QStringLiteral("模型（留空使用默认）"));
    modelLabel->setObjectName(QStringLiteral("detail"));
    auto* modelEdit = new QLineEdit;
    modelEdit->setPlaceholderText(QStringLiteral("model-name"));
    modelEdit->setText(AppSettings::aiModel());
    advancedLayout->addWidget(baseUrlLabel);
    advancedLayout->addWidget(baseUrlEdit);
    advancedLayout->addWidget(modelLabel);
    advancedLayout->addWidget(modelEdit);
    layout->addWidget(advancedWidget);

    auto* divider = new QFrame;
    divider->setObjectName(QStringLiteral("divider"));
    divider->setFixedHeight(1);
    layout->addWidget(divider);

    auto* statsBox = new QFrame;
    statsBox->setObjectName(QStringLiteral("statsBox"));
    auto* statsLayout = new QVBoxLayout(statsBox);
    statsLayout->setContentsMargins(10, 8, 10, 8);
    statsLayout->setSpacing(3);
    auto* statsTitle = new QLabel(QStringLiteral("用量统计"));
    statsTitle->setObjectName(QStringLiteral("detail"));
    auto* statsDetail = new QLabel(
        QStringLiteral("共解析 %1 次 · 输入 %2 tokens · 输出 %3 tokens")
            .arg(AppSettings::aiTotalRequests())
            .arg(AppSettings::aiTotalPromptTokens())
            .arg(AppSettings::aiTotalCompletionTokens()));
    statsDetail->setObjectName(QStringLiteral("detail"));
    statsLayout->addWidget(statsTitle);
    statsLayout->addWidget(statsDetail);
    layout->addWidget(statsBox);

    auto* buttonsRow = new QHBoxLayout;
    auto* cancelBtn = new QPushButton(QStringLiteral("取消"));
    auto* saveBtn = new QPushButton(QStringLiteral("保存"));
    saveBtn->setObjectName(QStringLiteral("primaryButton"));
    buttonsRow->addStretch();
    buttonsRow->addWidget(cancelBtn);
    buttonsRow->addWidget(saveBtn);
    layout->addLayout(buttonsRow);

    const auto updateProviderHints = [&](int idx) {
        const Provider& p = providers.at(idx);
        static const QHash<QString, QString> links = {
            {QStringLiteral("deepseek"),
             QStringLiteral("注册并获取 Key：<a style='color:#7ec8a8' href='https://platform.deepseek.com'>platform.deepseek.com</a>")},
            {QStringLiteral("aliyun"),
             QStringLiteral("注册并获取 Key：<a style='color:#7ec8a8' href='https://bailian.console.aliyun.com'>bailian.console.aliyun.com</a>")},
            {QStringLiteral("volcengine"),
             QStringLiteral("注册并获取 Key：<a style='color:#7ec8a8' href='https://www.volcengine.com/product/ark'>volcengine.com/ark</a>")},
            {QStringLiteral("zhipu"),
             QStringLiteral("注册并获取 Key：<a style='color:#7ec8a8' href='https://open.bigmodel.cn'>open.bigmodel.cn</a>")},
            {QStringLiteral("kimi"),
             QStringLiteral("注册并获取 Key：<a style='color:#7ec8a8' href='https://platform.moonshot.cn'>platform.moonshot.cn</a>")},
        };
        regLinkLabel->setText(links.value(p.id,
            QStringLiteral("填写自定义供应商的 Base URL 和模型名")));
        if (!p.baseUrl.isEmpty()) baseUrlEdit->setPlaceholderText(p.baseUrl);
        if (!p.defaultModel.isEmpty()) modelEdit->setPlaceholderText(p.defaultModel);
    };
    updateProviderHints(currentIdx);

    QObject::connect(providerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                     &dialog, [&](int idx) {
        updateProviderHints(idx);
        baseUrlEdit->setText(providers.at(idx).baseUrl);
        modelEdit->setText(providers.at(idx).defaultModel);
    });
    QObject::connect(advancedToggle, &QPushButton::clicked, &dialog, [&] {
        const bool visible = !advancedWidget->isVisible();
        advancedWidget->setVisible(visible);
        advancedToggle->setText(visible
            ? QStringLiteral("▼ 高级设置") : QStringLiteral("▶ 高级设置"));
        dialog.adjustSize();
    });
    QObject::connect(cancelBtn, &QPushButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(saveBtn, &QPushButton::clicked, &dialog, [&] {
        const QString key = keyEdit->text().trimmed();
        const Provider& p = providers.at(providerCombo->currentIndex());
        if (p.id == QStringLiteral("custom") &&
            (baseUrlEdit->text().trimmed().isEmpty() || modelEdit->text().trimmed().isEmpty())) {
            QMessageBox::warning(&dialog, QStringLiteral("配置不完整"),
                                 QStringLiteral("自定义供应商需要填写 Base URL 和模型名"));
            return;
        }
        AppSettings::setAiApiKey(key);
        AppSettings::setAiProvider(p.id);
        const QString typedUrl = baseUrlEdit->text().trimmed();
        AppSettings::setAiBaseUrl(typedUrl.isEmpty() ? p.baseUrl : typedUrl);
        const QString typedModel = modelEdit->text().trimmed();
        AppSettings::setAiModel(typedModel.isEmpty() ? p.defaultModel : typedModel);
        dialog.accept();
    });

    return dialog.exec() == QDialog::Accepted;
}


}  // namespace quizpane
