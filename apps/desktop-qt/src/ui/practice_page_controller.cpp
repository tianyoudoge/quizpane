#include "practice_page_controller.hpp"

#include "../app_settings.hpp"
#include "app_dialogs.hpp"
#include "line_icons.hpp"
#include "material_card.hpp"
#include "question_navigator.hpp"
#include "quizpane/draft_store.hpp"
#include "quizpane/pending_call.hpp"
#include "quizpane/provider_loader.hpp"
#include "quizpane/provider_response_router.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace quizpane {
namespace {

using ui::LineIcon;
using ui::makeLineIcon;

QString plainText(const QString& html) {
    // 选项文本只是 paragraph() 生成的简单 <p>/<span>/<br> 包裹 + toHtmlEscaped()
    // 转义，不需要 QTextDocument 的完整 HTML 解析和排版树构建——那是翻题时最重的
    // 单步开销，在 Win7 低配机上尤其明显。手动去标签 + 反转义即可等价还原纯文本。
    static const QRegularExpression tagPattern(QStringLiteral("<[^>]*>"));
    QString text = html;
    text.replace(QStringLiteral("<br>"), QStringLiteral("\n"), Qt::CaseInsensitive);
    text.remove(tagPattern);
    text.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
    text.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    text.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    text.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    text.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
    text.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    return text.trimmed();
}

int countAnswered(const QVector<QSet<int>>& answers) {
    return static_cast<int>(std::count_if(answers.cbegin(), answers.cend(),
        [](const QSet<int>& choices) { return !choices.isEmpty(); }));
}

void clearLayout(QLayout* layout) {
    while (layout && layout->count() > 0) {
        QLayoutItem* item = layout->takeAt(0);
        if (item->layout()) clearLayout(item->layout());
        delete item->widget();
        delete item;
    }
}

}  // namespace

void PracticePageController::init(
    QWidget* practicePage, QStackedWidget* pages, QWidget* catalogPage, QWidget* headerBar,
    QWidget* card, QWidget* resizeHandle, ProviderLoader& provider, DraftStore& draftStore,
    AttemptSession& session, UiSize& uiSize, QString& providerId, bool& draftRestoreChecked) {
    practicePage_ = practicePage; pages_ = pages; catalogPage_ = catalogPage;
    headerBar_ = headerBar; card_ = card; resizeHandle_ = resizeHandle; provider_ = &provider;
    draftStore_ = &draftStore; session_ = &session; uiSize_ = &uiSize; providerId_ = &providerId;
    draftRestoreChecked_ = &draftRestoreChecked;
}

void PracticePageController::buildInto() {
    // MainWindow may already have installed the course status bar in this layout.
    // Append the practice controls to it so every widget belongs to the same page.
    auto* practiceLayout = qobject_cast<QVBoxLayout*>(practicePage_->layout());
    if (!practiceLayout) practiceLayout = new QVBoxLayout(practicePage_);
    practiceLayout->setContentsMargins(0, 14, 0, 0);
    practiceLayout->setSpacing(10);
    practiceTitleLabel_ = new QLabel;
    practiceTitleLabel_->setObjectName(QStringLiteral("pageTitle"));
    practiceTitleLabel_->setWordWrap(true);
    practiceTitleLabel_->setVisible(false);
    practiceProgressLabel_ = new QLabel;
    practiceProgressLabel_->setObjectName(QStringLiteral("detail"));
    questionScroll_ = new QScrollArea;
    questionScroll_->setWidgetResizable(true);
    questionScroll_->setFrameShape(QFrame::NoFrame);
    questionScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    questionScroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    questionContent_ = new QWidget;
    questionContentLayout_ = new QVBoxLayout(questionContent_);
    questionContentLayout_->setContentsMargins(0, 4, 4, 4);
    practiceMaterialCard_ = new ui::MaterialCard;
    questionLabel_ = new QLabel;
    questionLabel_->setObjectName(QStringLiteral("questionText"));
    questionLabel_->setWordWrap(true);
    questionLabel_->setTextFormat(Qt::RichText);
    questionLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    optionsLayout_ = new QVBoxLayout;
    optionsLayout_->setSpacing(8);
    questionContentLayout_->addWidget(practiceMaterialCard_);
    questionContentLayout_->addWidget(questionLabel_);
    questionContentLayout_->addSpacing(10);
    questionContentLayout_->addLayout(optionsLayout_);
    questionContentLayout_->addStretch();
    questionScroll_->setWidget(questionContent_);
    practiceControlBar_ = new QWidget;
    practiceControlBar_->setObjectName(QStringLiteral("controlBar"));
    auto* questionNav = new QHBoxLayout(practiceControlBar_);
    questionNav->setContentsMargins(0, 0, 0, 0);
    previousQuestionButton_ = new QPushButton;
    nextQuestionButton_ = new QPushButton;
    questionListButton_ = new QPushButton;
    submitButton_ = new QPushButton;
    previousQuestionButton_->setIcon(makeLineIcon(LineIcon::Previous));
    nextQuestionButton_->setIcon(makeLineIcon(LineIcon::Next));
    questionListButton_->setIcon(makeLineIcon(LineIcon::QuestionList));
    submitButton_->setIcon(makeLineIcon(LineIcon::Submit));
    for (auto* button : {previousQuestionButton_, nextQuestionButton_, questionListButton_, submitButton_})
        button->setObjectName(QStringLiteral("navIconButton"));
    previousQuestionButton_->setAccessibleName(QStringLiteral("上一题"));
    previousQuestionButton_->setToolTip(QStringLiteral("上一题"));
    nextQuestionButton_->setAccessibleName(QStringLiteral("下一题"));
    nextQuestionButton_->setToolTip(QStringLiteral("下一题"));
    questionListButton_->setAccessibleName(QStringLiteral("选题清单"));
    questionListButton_->setToolTip(QStringLiteral("选题清单"));
    submitButton_->setAccessibleName(QStringLiteral("交卷"));
    submitButton_->setToolTip(QStringLiteral("交卷"));
    questionNav->addWidget(previousQuestionButton_);
    questionNav->addWidget(nextQuestionButton_);
    questionNav->addWidget(questionListButton_);
    questionNav->addStretch();
    questionNav->addWidget(submitButton_);
    QObject::connect(previousQuestionButton_, &QPushButton::clicked, practicePage_,
            [this] { showQuestion(currentQuestionIndex_ - 1); });
    QObject::connect(nextQuestionButton_, &QPushButton::clicked, practicePage_,
            [this] { showQuestion(currentQuestionIndex_ + 1); });
    QObject::connect(questionListButton_, &QPushButton::clicked,
            practicePage_, [this] { toggleQuestionNavigator(); });
    QObject::connect(submitButton_, &QPushButton::clicked, practicePage_, [this] { submitAttempt(); });
    practiceLayout->addWidget(practiceTitleLabel_);
    practiceLayout->addWidget(practiceProgressLabel_);
    practiceLayout->addWidget(questionScroll_);
    practiceLayout->addWidget(practiceControlBar_);

    questionNavigator_ = new ui::QuestionNavigator(practicePage_);
    QObject::connect(questionNavigator_, &ui::QuestionNavigator::questionSelected,
            practicePage_, [this](int index) { showQuestion(index); });

    // 非模态确认气泡是答题页的覆盖层，不进入主布局，因此出现时不会撑高窗口。
    submitConfirmationBubble_ = new QFrame(practicePage_);
    submitConfirmationBubble_->setObjectName(QStringLiteral("submitConfirmationBubble"));
    auto* confirmationLayout = new QVBoxLayout(submitConfirmationBubble_);
    confirmationLayout->setContentsMargins(10, 9, 10, 9);
    confirmationLayout->setSpacing(7);
    submitConfirmationLabel_ = new QLabel;
    submitConfirmationLabel_->setObjectName(QStringLiteral("submitConfirmationText"));
    submitConfirmationLabel_->setWordWrap(true);
    auto* confirmationActions = new QHBoxLayout;
    confirmationActions->setContentsMargins(0, 0, 0, 0);
    confirmationActions->addStretch();
    auto* cancelSubmitButton = new QPushButton(QStringLiteral("再检查一下"));
    cancelSubmitButton->setObjectName(QStringLiteral("confirmationSecondary"));
    auto* confirmSubmitButton = new QPushButton(QStringLiteral("交卷"));
    confirmSubmitButton->setObjectName(QStringLiteral("confirmationPrimary"));
    confirmationActions->addWidget(cancelSubmitButton);
    confirmationActions->addWidget(confirmSubmitButton);
    confirmationLayout->addWidget(submitConfirmationLabel_);
    confirmationLayout->addLayout(confirmationActions);
    submitConfirmationBubble_->setFixedWidth(232);
    submitConfirmationBubble_->hide();
    QObject::connect(cancelSubmitButton, &QPushButton::clicked,
            practicePage_, [this] { hideSubmitConfirmation(); });
    QObject::connect(confirmSubmitButton, &QPushButton::clicked,
            practicePage_, [this] { confirmSubmitAttempt(); });
}

QWidget* PracticePageController::controlBar() const { return practiceControlBar_; }

int PracticePageController::currentOptionCount() const {
    if (currentQuestionIndex_ < 0 || currentQuestionIndex_ >= session_->questions.size()) return 0;
    return static_cast<int>(
        session_->questions.at(currentQuestionIndex_).toObject().value("options").toArray().size());
}

bool PracticePageController::isCurrentPage() const {
    return pages_->currentWidget() == practicePage_;
}

void PracticePageController::startAttempt(const QString& categoryId, const QString& title,
                                          int count, bool includePreviouslyAnswered) {
    session_->attemptTitle = title;
    practiceProgressLabel_->setText(QStringLiteral("正在创建练习 · %1 题").arg(count));
    questionLabel_->setText(QStringLiteral("请稍候…"));
    clearLayout(optionsLayout_);
    lockedPracticeViewportHeight_ = 0;
    questionScroll_->setMinimumHeight(layout_metrics::kMinimumPracticeViewportHeight);
    pages_->setCurrentWidget(practicePage_);
    QString error;
    if (!provider_->request(
            makePendingCall(ProviderRoute::AttemptCreate,
                            QJsonObject{{"categoryId", categoryId}, {"count", count},
                                        {"includePreviouslyAnswered", includePreviouslyAnswered}}).build(),
            &error))
        QMessageBox::warning(practicePage_, QStringLiteral("无法创建练习"), error);
}

void PracticePageController::requestQuestions() {
    QString error;
    if (!provider_->request(
            makePendingCall(ProviderRoute::Questions,
                            QJsonObject{{"attemptId", session_->attemptId}}).build(), &error))
        QMessageBox::warning(practicePage_, QStringLiteral("无法读取题目"), error);
}

void PracticePageController::updateMaterialsCache(const QJsonArray& materials) {
    // attempt.questions 和 attempt.solutions 各自只返回当前作答范围内实际
    // 用到的材料（见 DeclarativeProvider::hostMaterials），这里合并进同一份
    // 缓存而不是整体替换，避免答题阶段收到的材料在进入解析页后丢失。
    for (const auto& value : materials) {
        const QJsonObject material = value.toObject();
        const QString materialId = material.value("id").toString();
        if (!materialId.isEmpty()) session_->materialsById.insert(materialId, material);
    }
}

void PracticePageController::showQuestion(int index) {
    if (session_->questions.isEmpty()) return;
    hideSubmitConfirmation();
    currentQuestionIndex_ = qBound(0, index, static_cast<int>(session_->questions.size()) - 1);
    const QJsonObject question = session_->questions.at(currentQuestionIndex_).toObject();
    practiceProgressLabel_->setText(QStringLiteral("第 %1 / %2 题 · 已作答 %3 题")
        .arg(currentQuestionIndex_ + 1).arg(session_->questions.size())
        .arg(countAnswered(session_->answers)));
    const QString materialId = question.value("materialId").toString();
    if (materialId.isEmpty()) {
        practiceMaterialCard_->hideMaterial();
    } else {
        const QJsonObject material = session_->materialsById.value(materialId);
        practiceMaterialCard_->showMaterial(materialId, material.value("title").toString(),
                                            material.value("contentHtml").toString(),
                                            material.value("imageUrls").toArray());
    }
    questionLabel_->setText(QStringLiteral("<div style=\"color:#d5d1c5\">%1</div>")
        .arg(question.value("contentHtml").toString()));
    clearLayout(optionsLayout_);
    const QJsonArray options = question.value("options").toArray();
    const bool multiple = question.value("type").toString() == QStringLiteral("multiple_choice");
    delete optionButtonGroup_;
    optionButtonGroup_ = multiple ? nullptr : new QButtonGroup(practicePage_);
    if (optionButtonGroup_)
        optionButtonGroup_->setExclusive(true);
    for (qsizetype optionIndex = 0; optionIndex < options.size(); ++optionIndex) {
        const QJsonObject option = options.at(optionIndex).toObject();
        // 不使用原生 RadioButton/CheckBox：macOS 会在指示器位置绘制半透明圆形
        // 选中浮层，而我们这里的选项字母已经是按钮文本的一部分，二者会产生错位。
        // QButtonGroup 继续负责单选互斥，多选则保留独立可切换行为。
        auto* button = static_cast<QAbstractButton*>(new QPushButton(QStringLiteral("%1. %2")
            .arg(option.value("label").toString(), plainText(option.value("contentHtml").toString()))));
        button->setCheckable(true);
        button->setObjectName(QStringLiteral("answerOption"));
        button->setChecked(session_->answers.value(currentQuestionIndex_).contains(optionIndex));
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        if (optionButtonGroup_)
            optionButtonGroup_->addButton(button);
        QObject::connect(button, &QAbstractButton::toggled, practicePage_,
                [this, optionIndex](bool checked) {
                    if (session_->questions.at(currentQuestionIndex_).toObject().value("type").toString() ==
                        QStringLiteral("multiple_choice")) chooseAnswer(static_cast<int>(optionIndex), checked);
                    else if (checked) chooseAnswer(static_cast<int>(optionIndex));
                });
        auto* optionCard = new QFrame;
        optionCard->setObjectName(QStringLiteral("answerOptionCard"));
        optionCard->setProperty("checked", button->isChecked());
        auto* optionLayout = new QVBoxLayout(optionCard);
        optionLayout->setContentsMargins(8, 7, 8, 8);
        optionLayout->setSpacing(4);
        optionLayout->addWidget(button);
        QObject::connect(button, &QAbstractButton::toggled, optionCard,
                [optionCard](bool checked) {
                    optionCard->setProperty("checked", checked);
                    optionCard->style()->unpolish(optionCard);
                    optionCard->style()->polish(optionCard);
                });
        const QString imageUrl = option.value("imageUrl").toString();
        if (!imageUrl.isEmpty()) {
            auto* image = new QLabel;
            image->setObjectName(QStringLiteral("answerOptionImage"));
            image->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            image->setAttribute(Qt::WA_TransparentForMouseEvents);
            QPixmap pixmap(QUrl(imageUrl).toLocalFile());
            if (!pixmap.isNull()) {
                image->setPixmap(pixmap.scaledToWidth(220, Qt::SmoothTransformation));
                image->setToolTip(QStringLiteral("选项 %1 的原卷图片").arg(option.value("label").toString()));
                optionLayout->addWidget(image);
            } else {
                image->deleteLater();
            }
        }
        optionsLayout_->addWidget(optionCard);
    }
    previousQuestionButton_->setEnabled(currentQuestionIndex_ > 0);
    nextQuestionButton_->setEnabled(currentQuestionIndex_ + 1 < session_->questions.size());
    questionScroll_->verticalScrollBar()->setValue(0);
    if (lockedPracticeViewportHeight_ == 0)
        QTimer::singleShot(0, practicePage_, [this] { lockCompactPracticeHeight(); });
    refreshQuestionNavigator();
    saveDraft();
}

void PracticePageController::chooseAnswer(int choice, bool checked) {
    if (currentQuestionIndex_ < 0 || currentQuestionIndex_ >= session_->answers.size()) return;
    const bool multiple = session_->questions.at(currentQuestionIndex_).toObject().value("type").toString() ==
        QStringLiteral("multiple_choice");
    if (multiple) {
        if (checked) session_->answers[currentQuestionIndex_].insert(choice);
        else session_->answers[currentQuestionIndex_].remove(choice);
    } else {
        session_->answers[currentQuestionIndex_] = QSet<int>{choice};
    }
    practiceProgressLabel_->setText(QStringLiteral("第 %1 / %2 题 · 已作答 %3 题")
        .arg(currentQuestionIndex_ + 1).arg(session_->questions.size())
        .arg(countAnswered(session_->answers)));
    refreshQuestionNavigator();
    const QJsonObject question = session_->questions.at(currentQuestionIndex_).toObject();
    QJsonValue savedChoice = QString::number(choice);
    if (multiple) { QJsonArray values; for (int value : session_->answers[currentQuestionIndex_]) values.append(QString::number(value)); savedChoice = values; }
    const QJsonArray payload{QJsonObject{
        {"questionIndex", currentQuestionIndex_},
        {"questionId", question.value("id").toString()},
        {"time", 0}, {"flag", 0},
        {"answer", QJsonObject{{"type", multiple ? 202 : 201}, {"choice", savedChoice}}}}};
    QString error;
    // request 只发起异步 RPC，不阻塞 UI；结果统一进入 handleProviderResponse()。
    provider_->request(
        makePendingCall(ProviderRoute::SaveAnswer,
                        QJsonObject{{"attemptId", session_->attemptId}, {"answers", payload}},
                        QStringLiteral("%1-%2").arg(currentQuestionIndex_).arg(choice)).build(),
        &error);
    saveDraft();
    // 留出选中反馈再自动进入下一题。原值 120ms 几乎是"一眨眼"，在职备考用户
    // 常想先点一个答案、再核对是否改主意，根本来不及回退；默认放宽到 700ms
    // 并允许在设置里调整（practice/autoAdvanceMs），设为 0 即关闭自动跳题。
    // 若用户已手动切题，index 校验会阻止旧定时器把新页面再次向前推进。
    const int advanceMs = AppSettings::autoAdvanceMs();
    const int answeredIndex = currentQuestionIndex_;
    if (multiple || advanceMs <= 0) return;
    if (answeredIndex + 1 < session_->questions.size()) {
        QTimer::singleShot(advanceMs, practicePage_, [this, answeredIndex] {
            if (currentQuestionIndex_ == answeredIndex)
                showQuestion(answeredIndex + 1);
        });
    } else {
        // 最后一题没有"下一题"可跳，短暂停留显示选中反馈后直接询问是否交卷。
        QTimer::singleShot(advanceMs, practicePage_, [this, answeredIndex] {
            if (currentQuestionIndex_ == answeredIndex)
                submitAttempt();
        });
    }
}

void PracticePageController::refreshQuestionNavigator() {
    if (!questionNavigator_) return;
    QSet<int> answered;
    for (int index = 0; index < session_->answers.size(); ++index) {
        if (!session_->answers.at(index).isEmpty()) answered.insert(index);
    }
    questionNavigator_->setState(static_cast<int>(session_->questions.size()), answered,
                                 currentQuestionIndex_);
}

void PracticePageController::toggleQuestionNavigator() {
    if (!questionNavigator_ || session_->questions.isEmpty()) return;
    if (questionNavigator_->isVisible()) {
        questionNavigator_->hide();
        return;
    }
    refreshQuestionNavigator();
    questionNavigator_->resize(300, qBound(150, questionNavigator_->sizeHint().height(), 320));
    const QPoint anchor = questionListButton_->mapToGlobal(
        QPoint(questionListButton_->width(), 0));
    QScreen* screen = QGuiApplication::screenAt(anchor);
    const QRect area = screen ? screen->availableGeometry()
                              : QRect(anchor - QPoint(320, 340), QSize(640, 680));
    int x = anchor.x() - questionNavigator_->width();
    int y = anchor.y() - questionNavigator_->height() - 8;
    if (y < area.top())
        y = questionListButton_->mapToGlobal(QPoint(0, questionListButton_->height())).y() + 8;
    x = qBound(area.left() + 4, x, area.right() - questionNavigator_->width() - 4);
    y = qBound(area.top() + 4, y, area.bottom() - questionNavigator_->height() - 4);
    questionNavigator_->move(x, y);
    questionNavigator_->show();
    questionNavigator_->raise();
}

QJsonArray PracticePageController::answerPayload() const {
    QJsonArray payload;
    for (qsizetype index = 0; index < session_->questions.size(); ++index) {
        const QSet<int> selected = session_->answers.value(index);
        const bool multiple = session_->questions.at(index).toObject().value("type").toString() == QStringLiteral("multiple_choice");
        QJsonValue choice;
        if (multiple) {
            QJsonArray selections; for (int value : selected) selections.append(QString::number(value));
            choice = selections;
        } else {
            choice = selected.isEmpty() ? QJsonValue(QJsonValue::Null)
                                        : QJsonValue(QString::number(*selected.constBegin()));
        }
        payload.append(QJsonObject{
            {"questionIndex", index},
            {"questionId", session_->questions.at(index).toObject().value("id").toString()},
            {"time", 0}, {"flag", 0},
            {"answer", QJsonObject{{"type", multiple ? 202 : 201}, {"choice", choice}}}});
    }
    return payload;
}

void PracticePageController::submitAttempt() {
    const int answered = countAnswered(session_->answers);
    const int unanswered = session_->answers.size() - answered;
    submitConfirmationLabel_->setText(unanswered > 0
        ? QStringLiteral("还有 %1 题未作答，确定交卷吗？").arg(unanswered)
        : QStringLiteral("已完成全部题目，确定交卷吗？"));
    submitConfirmationBubble_->adjustSize();
    positionSubmitConfirmation();
    submitConfirmationBubble_->show();
    submitConfirmationBubble_->raise();
}

void PracticePageController::hideSubmitConfirmation() {
    if (submitConfirmationBubble_) submitConfirmationBubble_->hide();
}

void PracticePageController::positionSubmitConfirmation() {
    if (!submitConfirmationBubble_ || !practiceControlBar_) return;
    const int x = qMax(4, practicePage_->width() -
                             submitConfirmationBubble_->width() - 4);
    const int y = qMax(4, practiceControlBar_->y() -
                             submitConfirmationBubble_->height() -
                                 layout_metrics::kSubmitBubbleInset);
    submitConfirmationBubble_->move(x, y);
}

void PracticePageController::confirmSubmitAttempt() {
    hideSubmitConfirmation();
    submitButton_->setEnabled(false);
    practiceProgressLabel_->setText(QStringLiteral("正在保存答案…"));
    QString error;
    if (!provider_->request(
            makePendingCall(ProviderRoute::FinalSave,
                            QJsonObject{{"attemptId", session_->attemptId},
                                        {"answers", answerPayload()}}).build(),
            &error)) {
        submitButton_->setEnabled(true);
        QMessageBox::warning(practicePage_, QStringLiteral("保存失败"), error);
    }
}

void PracticePageController::sendSubmit() {
    practiceProgressLabel_->setText(QStringLiteral("正在交卷…"));
    QString error;
    if (!provider_->request(
            makePendingCall(ProviderRoute::Submit,
                            QJsonObject{{"attemptId", session_->attemptId}}).build(), &error)) {
        submitButton_->setEnabled(true);
        QMessageBox::warning(practicePage_, QStringLiteral("交卷失败"), error);
    }
}

void PracticePageController::saveDraft() {
    if (providerId_->isEmpty() || session_->attemptId.isEmpty() || session_->questions.isEmpty()) return;
    // UI 当前状态先组装为 DTO，再交给 Core 持久化；控制器不关心文件路径和
    // 原子写入细节，这与 Controller -> Service 的分层方式一致。
    DraftSnapshot snapshot;
    snapshot.providerId = *providerId_;
    snapshot.attemptId = session_->attemptId;
    snapshot.title = session_->attemptTitle;
    snapshot.questions = session_->questions;
    for (auto it = session_->materialsById.constBegin(); it != session_->materialsById.constEnd(); ++it)
        snapshot.materials.append(it.value());
    // DraftSnapshot::answers 是旧的单选专用格式；多选题降级取集合首元素，
    // 与拆分前"answers_ 是单选源、multiAnswers_ 是多选源"时的草稿行为一致。
    snapshot.answers.reserve(session_->answers.size());
    for (const QSet<int>& choices : session_->answers)
        snapshot.answers.append(choices.isEmpty() ? -1 : *choices.constBegin());
    snapshot.currentQuestionIndex = currentQuestionIndex_;
    QString error;
    if (!draftStore_->save(snapshot, &error))
        qWarning("Unable to save draft: %s", qPrintable(error));
}

bool PracticePageController::maybeRestoreDraft() {
    if ((*draftRestoreChecked_) || providerId_->isEmpty()) return false;
    (*draftRestoreChecked_) = true;
    QString error;
    QList<DraftSnapshot> drafts = draftStore_->list(*providerId_, &error);
    while (!drafts.isEmpty()) {
        const ui::DraftDecision decision = ui::chooseDraft(practicePage_, drafts);
        if (decision.choice == ui::DraftChoice::Later) return false;
        if (decision.index < 0 || decision.index >= drafts.size()) return false;
        if (decision.choice == ui::DraftChoice::Discard) {
            draftStore_->clearAttempt(*providerId_, drafts.at(decision.index).attemptId, &error);
            drafts.removeAt(decision.index);
            continue;
        }
        const DraftSnapshot snapshot = drafts.at(decision.index);
        session_->attemptId = snapshot.attemptId;
        session_->attemptTitle = snapshot.title;
        session_->questions = snapshot.questions;
        updateMaterialsCache(snapshot.materials);
        session_->answers.clear();
        session_->answers.reserve(static_cast<int>(session_->questions.size()));
        for (int index = 0; index < session_->questions.size(); ++index) {
            const int restored = index < snapshot.answers.size() ? snapshot.answers.at(index) : -1;
            session_->answers.append(restored >= 0 ? QSet<int>{restored} : QSet<int>{});
        }
        lockedPracticeViewportHeight_ = 0;
        submitButton_->setEnabled(true);
        pages_->setCurrentWidget(practicePage_);
        showQuestion(qBound(0, snapshot.currentQuestionIndex,
                            static_cast<int>(session_->questions.size()) - 1));
        return true;
    }
    return false;
}

int PracticePageController::answerViewportMaximumHeight() const {
    if (*uiSize_ == UiSize::Small) return 260;
    if (*uiSize_ == UiSize::Large) return 480;
    return 360;
}

void PracticePageController::lockCompactPracticeHeight() {
    if (pages_->currentWidget() != practicePage_ || session_->questions.isEmpty()) return;
    questionContentLayout_->activate();
    const int contentHeight = questionContentLayout_->sizeHint().height() + 4;
    lockedPracticeViewportHeight_ = qBound(150, contentHeight,
                                           answerViewportMaximumHeight());
    questionScroll_->setMinimumHeight(layout_metrics::kMinimumPracticeViewportHeight);
    questionScroll_->verticalScrollBar()->setSingleStep(24);

    const auto* root = qobject_cast<QVBoxLayout*>(card_->layout());
    const auto* practice = qobject_cast<QVBoxLayout*>(practicePage_->layout());
    if (!root || !practice) return;
    const QMargins margins = root->contentsMargins();
    const QMargins pageMargins = practice->contentsMargins();
    const int fixedHeight = margins.top() + margins.bottom() +
        headerBar_->height() + (resizeHandle_ ? resizeHandle_->height() : 0) + root->spacing() * 2 +
        pageMargins.top() + pageMargins.bottom() +
        practiceProgressLabel_->sizeHint().height() +
        practiceControlBar_->height() + practice->spacing() * 2;
    const int totalHeight = fixedHeight + lockedPracticeViewportHeight_;
    const QPoint oldTopRight = practicePage_->window()->frameGeometry().topRight();
    practicePage_->window()->resize(practicePage_->window()->width(), totalHeight);
    if (practicePage_->window()->isVisible())
        practicePage_->window()->move(oldTopRight.x() - practicePage_->window()->width() + 1, oldTopRight.y());
}

void PracticePageController::handleResize() {
    if (submitConfirmationBubble_ && submitConfirmationBubble_->isVisible())
        positionSubmitConfirmation();
    if (pages_->currentWidget() != practicePage_ || session_->questions.isEmpty() || !questionScroll_) return;
    const auto* root = qobject_cast<QVBoxLayout*>(card_->layout());
    const auto* practice = qobject_cast<QVBoxLayout*>(practicePage_->layout());
    if (!root || !practice) return;
    const QMargins rootMargins = root->contentsMargins();
    const QMargins pageMargins = practice->contentsMargins();
    const int fixedHeight = rootMargins.top() + rootMargins.bottom() +
        headerBar_->height() + (resizeHandle_ ? resizeHandle_->height() : 0) + root->spacing() * 2 +
        pageMargins.top() + pageMargins.bottom() +
        practiceProgressLabel_->sizeHint().height() +
        practiceControlBar_->height() + practice->spacing() * 2;
    const int viewportHeight = qMax(layout_metrics::kMinimumPracticeViewportHeight,
                                    practicePage_->window()->height() - fixedHeight);
    lockedPracticeViewportHeight_ = viewportHeight;
}

void PracticePageController::resetForNewQuestions() {
    session_->answers.clear();
    session_->answers.resize(static_cast<int>(session_->questions.size()));
    submitButton_->setEnabled(true);
    showQuestion(0);
    pages_->setCurrentWidget(practicePage_);
    saveDraft();
}

void PracticePageController::setSubmitEnabled(bool enabled) {
    if (submitButton_) submitButton_->setEnabled(enabled);
}

}  // namespace quizpane
