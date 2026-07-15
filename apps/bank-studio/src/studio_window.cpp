#include "studio_window.hpp"
#include "quizpane/diagnostic_logger.hpp"

#include "quizpane/bank_validator.hpp"
#include "quizpane/declarative_provider.hpp"
#include "quizpane/provider_installer.hpp"
#include "quizpane/studio/generation_workflow.hpp"
#include "quizpane/zip_archive.hpp"
#include "source_row_widget.hpp"
#include "source_validation.hpp"
#include "styled_dropdown.hpp"

#include <QCloseEvent>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDragEnterEvent>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDropEvent>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QMenuBar>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStyle>
#include <QSet>
#include <QTimer>
#include <QSettings>
#include <QProcess>
#include <QTreeWidget>
#include <QTemporaryDir>
#include <QUrl>
#include <QVBoxLayout>

namespace quizpane::studio {
namespace {

// 仅供题库制作器页面复用的轻量控件工厂。返回的控件在加入布局后由 Qt 父子
// 对象树托管，调用方不需要手工 delete。
QLabel* mutedLabel(const QString& text) {
    auto* label = new QLabel(text);
    label->setObjectName(QStringLiteral("muted"));
    label->setWordWrap(true);
    return label;
}

QFrame* metricCard(const QString& name, QLabel** value) {
    auto* card = new QFrame;
    card->setObjectName(QStringLiteral("metricCard"));
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(4);
    layout->addWidget(mutedLabel(name));
    *value = new QLabel(QStringLiteral("0"));
    (*value)->setObjectName(QStringLiteral("metricValue"));
    layout->addWidget(*value);
    return card;
}

bool confirmAction(QWidget* parent, const QString& title, const QString& text,
                   const QString& acceptText) {
    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    auto* layout = new QVBoxLayout(&dialog);
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    auto* buttons = new QDialogButtonBox;
    auto* cancel = buttons->addButton(QStringLiteral("取消"), QDialogButtonBox::RejectRole);
    auto* accept = buttons->addButton(acceptText, QDialogButtonBox::AcceptRole);
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(accept, &QPushButton::clicked, &dialog, &QDialog::accept);
    layout->addWidget(label); layout->addWidget(buttons);
    dialog.setMinimumWidth(360);
    return dialog.exec() == QDialog::Accepted;
}

enum class TaskCloseChoice { Keep, Delete, Cancel };
TaskCloseChoice chooseTaskCloseAction(QWidget* parent) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("保留生成任务？"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* label = new QLabel(QStringLiteral(
        "任务尚未完成。保留后，下次添加同一批文件可继续；删除会移除本次检查点。"));
    label->setWordWrap(true);
    auto* buttons = new QDialogButtonBox;
    auto* cancel = buttons->addButton(QStringLiteral("取消关闭"), QDialogButtonBox::RejectRole);
    auto* remove = buttons->addButton(QStringLiteral("删除任务"), QDialogButtonBox::DestructiveRole);
    auto* keep = buttons->addButton(QStringLiteral("保留并关闭"), QDialogButtonBox::AcceptRole);
    TaskCloseChoice result = TaskCloseChoice::Cancel;
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(remove, &QPushButton::clicked, &dialog, [&] { result = TaskCloseChoice::Delete; dialog.accept(); });
    QObject::connect(keep, &QPushButton::clicked, &dialog, [&] { result = TaskCloseChoice::Keep; dialog.accept(); });
    layout->addWidget(label); layout->addWidget(buttons);
    dialog.exec();
    return result;
}

}  // namespace

// ===== 应用外壳与四步向导装配 =====

StudioWindow::StudioWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("题库制作器 · 小窗刷题"));
    setMinimumSize(820, 600);
    resize(1040, 720);
    setAcceptDrops(true);

    auto* root = new QWidget;
    setCentralWidget(root);
    auto* rootLayout = new QHBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto* sidebar = new QFrame;
    sidebar->setObjectName(QStringLiteral("sidebar"));
    sidebar->setFixedWidth(224);
    auto* sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(22, 24, 18, 20);
    sideLayout->setSpacing(9);
    auto* brand = new QLabel(QStringLiteral("题库制作器"));
    brand->setObjectName(QStringLiteral("brand"));
    sideLayout->addWidget(brand);
    sideLayout->addWidget(mutedLabel(QStringLiteral("把你的文档整理成可安装题库")));
    sideLayout->addSpacing(22);
    const QStringList steps{QStringLiteral("01  选择资料"), QStringLiteral("02  自动整理"),
        QStringLiteral("03  检查问题"), QStringLiteral("04  完成")};
    for (int index = 0; index < steps.size(); ++index) {
        auto* step = new QLabel(steps.at(index));
        step->setObjectName(QStringLiteral("sideStep"));
        step->setProperty("stepIndex", index);
        sideLayout->addWidget(step);
    }
    sideLayout->addStretch();
    auto* privacy = mutedLabel(
        QStringLiteral("原文件默认只在本机读取。调用云端模型前会再次提示上传范围。"));
    privacy->setObjectName(QStringLiteral("privacyFootnote"));
    sideLayout->addWidget(privacy);
    rootLayout->addWidget(sidebar);

    auto* content = new QWidget;
    content->setObjectName(QStringLiteral("content"));
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(34, 26, 34, 24);
    contentLayout->setSpacing(18);
    pages_ = new QStackedWidget;
    pages_->addWidget(buildSourcePage());
    pages_->addWidget(buildProgressPage());
    pages_->addWidget(buildReviewPage());
    pages_->addWidget(buildFinishPage());
    contentLayout->addWidget(pages_, 1);

    auto* navigation = new QHBoxLayout;
    backButton_ = new QPushButton(QStringLiteral("上一步"));
    backButton_->setObjectName(QStringLiteral("secondaryButton"));
    nextButton_ = new QPushButton(QStringLiteral("下一步"));
    startButton_ = new QPushButton(QStringLiteral("开始整理"));
    startButton_->setObjectName(QStringLiteral("primaryButton"));
    navigation->addWidget(backButton_);
    navigation->addStretch();
    navigation->addWidget(nextButton_);
    navigation->addWidget(startButton_);
    contentLayout->addLayout(navigation);
    rootLayout->addWidget(content, 1);

    connect(backButton_, &QPushButton::clicked, this, [this] { movePage(-1); });
    connect(nextButton_, &QPushButton::clicked, this, [this] { movePage(1); });
    connect(startButton_, &QPushButton::clicked, this, &StudioWindow::beginPreflight);
    connect(pages_, &QStackedWidget::currentChanged, this, &StudioWindow::updateNavigation);
    connect(generationMode_, &StyledDropdown::currentIndexChanged, this, [this] {
        if (!modelSummary_) return;
        if (generationMode_->currentData().toString() == QStringLiteral("rules")) {
            modelSummary_->setText(
                QStringLiteral("当前方式：离线整理 · 资料不会离开电脑"));
        } else {
            modelSummary_->setText(
                QStringLiteral("当前模型：%1 · %2（可在“设置 → 模型设置”中修改）")
                    .arg(modelSettings_.serviceName, modelSettings_.modelName));
        }
    });
    networkManager_ = new QNetworkAccessManager(this);
    auto* settingsMenu = menuBar()->addMenu(QStringLiteral("设置"));
    auto* modelSettingsAction = settingsMenu->addAction(
        QStringLiteral("模型设置…"), this, &StudioWindow::showModelSettings);
    modelSettingsAction->setShortcut(QKeySequence::Preferences);
#ifdef QUIZPANE_DIAGNOSTIC_LOGGING
    settingsMenu->addAction(QStringLiteral("查看调试日志…"), this, [] {
        diagnostic::openLogFile();
    });
#endif
    applyStyle();
    updateNavigation();
}

QWidget* StudioWindow::pageHeader(const QString& eyebrow, const QString& title,
                                  const QString& description) {
    auto* widget = new QWidget;
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    auto* eyebrowLabel = new QLabel(eyebrow);
    eyebrowLabel->setObjectName(QStringLiteral("eyebrow"));
    auto* titleLabel = new QLabel(title);
    titleLabel->setObjectName(QStringLiteral("pageTitle"));
    layout->addWidget(eyebrowLabel);
    layout->addWidget(titleLabel);
    layout->addWidget(mutedLabel(description));
    return widget;
}

// ===== 第 1～4 步页面构建 =====

QWidget* StudioWindow::buildSourcePage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(16);
    layout->addWidget(pageHeader(
        QStringLiteral("第一步"), QStringLiteral("添加题目资料"),
        QStringLiteral("支持 TXT、Markdown、DOCX 和 PDF。题目和答案分在两个文件里也可以一起整理。")));
    auto* modePanel = new QFrame;
    modePanel->setObjectName(QStringLiteral("panel"));
    auto* modeLayout = new QVBoxLayout(modePanel);
    modeLayout->setContentsMargins(16, 12, 16, 12);
    modeLayout->addWidget(new QLabel(QStringLiteral("整理方式")));
    generationMode_ = new StyledDropdown;
    generationMode_->addItem(QStringLiteral("离线整理（推荐）"), QStringLiteral("rules"));
    generationMode_->addItem(QStringLiteral("AI 辅助整理"), QStringLiteral("model"));
    modeLayout->addWidget(generationMode_);
    auto* modeHint = mutedLabel(QString());
    modeLayout->addWidget(modeHint);
    const auto refreshModeHint = [modeHint](const QString& mode) {
        modeHint->setText(mode == QStringLiteral("model")
            ? QStringLiteral("AI 辅助整理更适合排版复杂的资料，会使用你配置的模型，结果需要复核。")
            : QStringLiteral("离线整理不会上传资料，适合题号、选项和答案比较规范的文档。"));
    };
    refreshModeHint(generationMode_->currentData().toString());
    connect(generationMode_, &StyledDropdown::currentIndexChanged, this,
        [this, refreshModeHint] { refreshModeHint(generationMode_->currentData().toString()); });
    layout->addWidget(modePanel);
    auto* drop = new QFrame;
    drop->setObjectName(QStringLiteral("dropZone"));
    auto* dropLayout = new QVBoxLayout(drop);
    dropLayout->setContentsMargins(22, 24, 22, 24);
    auto* dropTitle = new QLabel(QStringLiteral("拖入文件，或从电脑中选择"));
    dropTitle->setObjectName(QStringLiteral("sectionTitle"));
    dropTitle->setAlignment(Qt::AlignCenter);
    auto* addButton = new QPushButton(QStringLiteral("添加题目或资料"));
    addButton->setObjectName(QStringLiteral("primaryButton"));
    addButton->setFixedWidth(120);
    dropLayout->addWidget(dropTitle);
    auto* ocrHint = mutedLabel(QStringLiteral(
        "扫描版 PDF 会在本机识别文字，处理时间可能稍长。"));
    ocrHint->setAlignment(Qt::AlignCenter);
    dropLayout->addWidget(ocrHint);
    dropLayout->addWidget(addButton, 0, Qt::AlignHCenter);
    layout->addWidget(drop);
    connect(addButton, &QPushButton::clicked, this, &StudioWindow::addSourceFiles);

    sourcePanel_ = new QWidget;
    auto* sourcePanelLayout = new QVBoxLayout(sourcePanel_);
    sourcePanelLayout->setContentsMargins(0, 0, 0, 0);
    sourcePanelLayout->setSpacing(8);
    auto* listHeader = new QHBoxLayout;
    auto* listTitle = new QLabel(QStringLiteral("已添加资料"));
    listTitle->setObjectName(QStringLiteral("sectionTitle"));
    sourceSummary_ = mutedLabel(QStringLiteral("尚未添加文件"));
    listHeader->addWidget(listTitle);
    listHeader->addWidget(sourceSummary_);
    listHeader->addStretch();
    sourcePanelLayout->addLayout(listHeader);

    sourceScroll_ = new QScrollArea;
    sourceScroll_->setObjectName(QStringLiteral("sourceScroll"));
    sourceScroll_->setWidgetResizable(true);
    sourceScroll_->setFrameShape(QFrame::NoFrame);
    sourceScroll_->setMaximumHeight(320);
    auto* sourceListContent = new QWidget;
    sourceListLayout_ = new QVBoxLayout(sourceListContent);
    sourceListLayout_->setContentsMargins(0, 4, 4, 4);
    sourceListLayout_->setSpacing(10);
    sourceListLayout_->addStretch();
    sourceScroll_->setWidget(sourceListContent);
    sourcePanelLayout->addWidget(sourceScroll_);

    sourcePanel_->setVisible(false);
    layout->addWidget(sourcePanel_);
    layout->addStretch();
    return page;
}

QWidget* StudioWindow::buildProgressPage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(18);
    layout->addWidget(pageHeader(
        QStringLiteral("第二步"), QStringLiteral("整理题目"),
        QStringLiteral("会先读取资料并检查题目结构；关闭窗口前会询问是否保留未完成任务。")));
    modelSummary_ = mutedLabel(
        QStringLiteral("当前方式：离线整理 · 资料不会离开电脑"));
    modelSummary_->setObjectName(QStringLiteral("notice"));
    layout->addWidget(modelSummary_);
    phaseLabel_ = new QLabel(QStringLiteral("等待开始"));
    phaseLabel_->setObjectName(QStringLiteral("phaseTitle"));
    phaseDetail_ =
        mutedLabel(QStringLiteral("点击下方“开始整理”后，先在本地检查资料。"));
    activitySpinner_ = new QLabel(QStringLiteral("◐ 运行中"));
    activitySpinner_->setObjectName(QStringLiteral("activitySpinner"));
    activitySpinner_->hide();
    activityTimer_ = new QTimer(this);
    connect(activityTimer_, &QTimer::timeout, this, [this] {
        static const QStringList frames{QStringLiteral("◐"), QStringLiteral("◓"),
                                        QStringLiteral("◑"), QStringLiteral("◒")};
        activitySpinner_->setText(frames.at(spinnerFrame_++ % frames.size()) +
                                  QStringLiteral(" 运行中"));
    });
    progressBar_ = new QProgressBar;
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    progressBar_->setTextVisible(false);
    layout->addWidget(phaseLabel_);
    layout->addWidget(phaseDetail_);
    layout->addWidget(activitySpinner_);
    layout->addWidget(progressBar_);
    auto* metrics = new QHBoxLayout;
    metrics->addWidget(metricCard(QStringLiteral("已读取资料"), &inputTokens_));
    metrics->addWidget(metricCard(QStringLiteral("已整理题目"), &outputTokens_));
    metrics->addWidget(metricCard(QStringLiteral("待复核"), &totalTokens_));
    layout->addLayout(metrics);
    auto* stages = new QFrame;
    stages->setObjectName(QStringLiteral("panel"));
    auto* stagesLayout = new QVBoxLayout(stages);
    stagesLayout->setContentsMargins(18, 16, 18, 16);
    stagesLayout->addWidget(new QLabel(QStringLiteral("处理阶段")));
    stagesLayout->addWidget(mutedLabel(
        QStringLiteral("读取资料  →  识别题目、选项和答案  →  检查结果")));
    layout->addWidget(stages);
    layout->addStretch();
    return page;
}

QWidget* StudioWindow::buildReviewPage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(16);
    layout->addWidget(pageHeader(
        QStringLiteral("第三步"), QStringLiteral("只检查需要你决定的问题"),
        QStringLiteral("正常题目会自动收起。这里集中展示缺答案、疑似重复和选项错位。")));
    auto* filters = new QHBoxLayout;
    allReviewButton_ = new QPushButton(QStringLiteral("全部异常  0"));
    missingAnswerButton_ = new QPushButton(QStringLiteral("缺少答案  0"));
    duplicateButton_ = new QPushButton(QStringLiteral("疑似重复  0"));
    filters->addWidget(allReviewButton_);
    filters->addWidget(missingAnswerButton_);
    filters->addWidget(duplicateButton_);
    filters->addStretch();
    layout->addLayout(filters);
    reviewTree_ = new QTreeWidget;
    reviewTree_->setColumnCount(3);
    reviewTree_->setHeaderLabels({QStringLiteral("材料 / 题目"), QStringLiteral("问题"),
                                   QStringLiteral("内容预览")});
    reviewTree_->header()->setStretchLastSection(true);
    reviewTree_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    reviewTree_->setAlternatingRowColors(false);
    reviewTree_->setRootIsDecorated(true);
    layout->addWidget(reviewTree_, 1);
    return page;
}

QWidget* StudioWindow::buildFinishPage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(16);
    layout->addWidget(pageHeader(
        QStringLiteral("第四步"), QStringLiteral("生成并添加到小窗刷题"),
        QStringLiteral("确认题库名称和组卷方式。完成后会自动添加到小窗刷题，无需再手动导入。")));
    auto* panel = new QFrame;
    panel->setObjectName(QStringLiteral("panel"));
    auto* form = new QVBoxLayout(panel);
    form->setContentsMargins(20, 18, 20, 18);
    form->addWidget(new QLabel(QStringLiteral("题库名称")));
    bankName_ = new QLineEdit;
    bankName_->setPlaceholderText(QStringLiteral("例如：我的行测常识题库"));
    form->addWidget(bankName_);
    form->addWidget(new QLabel(QStringLiteral("默认每套题数量")));
    questionCount_ = new StyledDropdown;
    questionCount_->addItems({QStringLiteral("5 题"), QStringLiteral("10 题"),
                     QStringLiteral("15 题"), QStringLiteral("全部题目")});
    form->addWidget(questionCount_);
    finishPath_ = mutedLabel(QStringLiteral("完成后会自动保存到小窗刷题的题库目录。"));
    form->addWidget(finishPath_);
    layout->addWidget(panel);
    layout->addStretch();
    return page;
}

// ===== 资料列表与模型设置 =====

void StudioWindow::addSourceFiles() {
    appendSources(QFileDialog::getOpenFileNames(this, QStringLiteral("添加题目或资料"), {},
        QStringLiteral("题目资料 (*.txt *.md *.markdown *.docx *.pdf)")));
}

void StudioWindow::appendSources(const QStringList& paths) {
    int added = 0;
    for (const QString& path : paths) {
        const QString absolute = QFileInfo(path).absoluteFilePath();
        if (!acceptedSource(absolute) || sourcePaths_.contains(absolute)) continue;
        sourcePaths_.append(absolute);
        ++added;
        auto* row = new SourceRowWidget(absolute);
        sourceRows_.insert(absolute, row);
        // 插入到末尾的拉伸占位之前，保持新行始终追加在列表最下方。
        sourceListLayout_->insertWidget(sourceListLayout_->count() - 1, row);
        connect(row, &SourceRowWidget::answerRequested, this, [this, absolute] {
            const QString answer = QFileDialog::getOpenFileName(
                this, QStringLiteral("添加答案或解析"), {},
                QStringLiteral("答案或解析 (*.txt *.md *.markdown *.docx *.pdf)"));
            if (answer.isEmpty()) return;
            pairAnswer(absolute, QFileInfo(answer).absoluteFilePath());
        });
        connect(row, &SourceRowWidget::answerDropped, this, [this, absolute](const QString& answer) {
            pairAnswer(absolute, answer);
        });
        connect(row, &SourceRowWidget::answerCleared, this, [this, absolute] {
            answerPathsByQuestion_.remove(absolute);
            if (auto* r = sourceRows_.value(absolute)) r->clearPairedAnswer();
        });
        connect(row, &SourceRowWidget::removeRequested, this, [this, absolute] {
            removeSource(absolute);
        });
    }
    diagnostic::event(QStringLiteral("studio"), QStringLiteral("sources-updated"),
        {{QStringLiteral("offered"), paths.size()},
         {QStringLiteral("added"), added},
         {QStringLiteral("total"), sourcePaths_.size()}});
    sourceSummary_->setText(sourcePaths_.isEmpty() ? QStringLiteral("尚未添加文件")
        : QStringLiteral("%1 个文件").arg(sourcePaths_.size()));
    sourcePanel_->setVisible(!sourcePaths_.isEmpty());
    updateNavigation();
}

void StudioWindow::pairAnswer(const QString& question, const QString& answer) {
    if (answer == question || !acceptedSource(answer)) return;
    answerPathsByQuestion_.insert(question, answer);
    if (auto* row = sourceRows_.value(question)) row->setPairedAnswer(answer);
}

void StudioWindow::removeSource(const QString& question) {
    sourcePaths_.removeAll(question);
    answerPathsByQuestion_.remove(question);
    if (auto* row = sourceRows_.take(question)) {
        sourceListLayout_->removeWidget(row);
        row->deleteLater();
    }
    sourceSummary_->setText(sourcePaths_.isEmpty() ? QStringLiteral("尚未添加文件")
        : QStringLiteral("%1 个文件").arg(sourcePaths_.size()));
    sourcePanel_->setVisible(!sourcePaths_.isEmpty());
    updateNavigation();
}

void StudioWindow::showModelSettings() {
    // 对话框返回新的完整 DTO；用户取消时保持当前设置不变。
    const auto edited = editModelSettings(this, modelSettings_);
    if (!edited) return;

    modelSettings_ = *edited;
    if (modelSummary_ && generationMode_->currentData().toString() == QStringLiteral("model")) {
        modelSummary_->setText(
            QStringLiteral("当前模型：%1 · %2（可在“设置 → 模型设置”中修改）")
                .arg(modelSettings_.serviceName, modelSettings_.modelName));
    }
}

// ===== 向导状态机与本地预检 =====

void StudioWindow::movePage(int delta) {
    pages_->setCurrentIndex(qBound(0, pages_->currentIndex() + delta, pages_->count() - 1));
}

void StudioWindow::updateNavigation() {
    const int page = pages_->currentIndex();
    backButton_->setVisible(page > 0);
    nextButton_->setVisible(page == 0 || page == 2);
    startButton_->setVisible(page == 1 || page == 3);
    nextButton_->setEnabled(page != 0 || !sourcePaths_.isEmpty());
    startButton_->setEnabled(true);
    startButton_->setText(page == 3 ? QStringLiteral("生成题库安装包")
                                    : QStringLiteral("开始整理"));
    const auto steps = findChildren<QLabel*>(QStringLiteral("sideStep"));
    for (QLabel* step : steps) {
        step->setProperty("active", step->property("stepIndex").toInt() == page);
        step->style()->unpolish(step); step->style()->polish(step);
    }
}

void StudioWindow::beginPreflight() {
    if (pages_->currentIndex() == 3) {
        packageProvider();
        return;
    }
    if (sourcePaths_.isEmpty()) return;
    const bool ruleBased =
        generationMode_->currentData().toString() == QStringLiteral("rules");
    diagnostic::event(QStringLiteral("studio"), QStringLiteral("generation-start"),
        {{QStringLiteral("mode"), ruleBased ? QStringLiteral("rules") : QStringLiteral("model")},
         {QStringLiteral("sources"), sourcePaths_.size()},
         {QStringLiteral("vendor"), ruleBased ? QString{} : modelSettings_.vendorId},
         {QStringLiteral("model"), ruleBased ? QString{} : modelSettings_.modelName}});
    if (!ruleBased && modelSettings_.vendorId == QStringLiteral("anthropic")) {
        QMessageBox::information(this, QStringLiteral("暂未实现"),
            QStringLiteral("Anthropic Messages API 生成暂未实现。请选择 OpenAI 兼容供应商。"));
        return;
    }
    if (workflow_ && workflow_->isActive()) return;
    if (workflow_) workflow_->deleteLater();
    workflow_ = new GenerationWorkflow(networkManager_, this);
    connect(workflow_, &GenerationWorkflow::progressChanged,
            this, &StudioWindow::updateWorkflowProgress);
    connect(workflow_, &GenerationWorkflow::questionsReady,
            this, &StudioWindow::populateReview);
    connect(workflow_, &GenerationWorkflow::failed, this, [this](const QString& error) {
        activityTimer_->stop();
        activitySpinner_->hide();
        startButton_->setEnabled(true);
        if (error.contains(QStringLiteral("重新开始"))) {
            if (confirmAction(this, QStringLiteral("重新开始生成任务？"),
                              error + QStringLiteral("\n\n是否删除旧检查点并重新开始？"),
                              QStringLiteral("删除并重新开始"))) {
                CheckpointStore store;
                QString clearError;
                if (!store.clear(store.taskIdForSources(sourcePaths_), &clearError)) {
                    QMessageBox::warning(this, QStringLiteral("无法删除旧任务"), clearError);
                    return;
                }
                QTimer::singleShot(0, this, &StudioWindow::beginPreflight);
            }
            return;
        }
        QMessageBox::warning(this, QStringLiteral("生成未完成"), error);
    });
    connect(workflow_, &GenerationWorkflow::finished, this, [this] {
        activityTimer_->stop();
        activitySpinner_->hide();
        startButton_->setEnabled(true);
        if (generatedQuestions_.isEmpty() && reviewQuestions_.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("没有生成题目"),
                                 QStringLiteral("没有从资料中识别出可用题目。"));
            return;
        }
        pages_->setCurrentIndex(2);
    });
    progressBar_->setValue(0);
    inputTokens_->setText(QString::number(sourcePaths_.size()));
    outputTokens_->setText(QStringLiteral("0"));
    totalTokens_->setText(QStringLiteral("0"));
    startButton_->setEnabled(false);
    spinnerFrame_ = 0;
    activitySpinner_->setText(QStringLiteral("◐ 运行中"));
    activitySpinner_->show();
    activityTimer_->start(120);
    if (ruleBased) {
        QList<SourceMaterialGroup> groups;
        for (const QString& question : sourcePaths_)
            groups.append({question, answerPathsByQuestion_.value(question)});
        workflow_->startRuleBased(groups);
    }
    else {
        QStringList modelSources = sourcePaths_;
        // AI 路径也把配对的答案/解析一并送入，但仍保留文件名，提示词能够区分
        // 两份来源；离线路径则使用更严格的按题号合并规则。
        for (const QString& question : sourcePaths_) {
            const QString answer = answerPathsByQuestion_.value(question);
            if (!answer.isEmpty()) modelSources.append(answer);
        }
        workflow_->start(modelSources, modelSettings_);
    }
}

void StudioWindow::updateWorkflowProgress(const WorkflowProgress& progress) {
    int base = 0, span = 0;
    QString phase;
    switch (progress.stage) {
    case WorkflowStage::Extracting: base = 5; span = 20; phase = QStringLiteral("读取资料"); break;
    case WorkflowStage::Chunking: base = 25; span = 5; phase = QStringLiteral("整理内容"); break;
    case WorkflowStage::Generating: base = 30; span = 40; phase = QStringLiteral("AI 整理"); break;
    case WorkflowStage::Validating: base = 70; span = 15; phase = QStringLiteral("检查结果"); break;
    case WorkflowStage::Repairing: base = 85; span = 10; phase = QStringLiteral("调整结果"); break;
    case WorkflowStage::Packaging: base = 95; span = 5; phase = QStringLiteral("打包"); break;
    case WorkflowStage::Done: base = 100; phase = QStringLiteral("整理完成"); break;
    case WorkflowStage::Failed: phase = QStringLiteral("任务中断"); break;
    default: phase = QStringLiteral("等待继续"); break;
    }
    const int within = progress.totalSourceBlocks > 0
        ? span * progress.completedSourceBlocks / progress.totalSourceBlocks : 0;
    progressBar_->setValue(qBound(0, base + within, 100));
    phaseLabel_->setText(phase);
    phaseDetail_->setText(progress.detail);
    inputTokens_->setText(QString::number(progress.completedSourceBlocks));
}

void StudioWindow::populateReview(const GeneratedBankCandidate& candidate) {
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
    generatedMaterials_ = candidate.materials;
    generatedQuestions_ = candidate.questions;
    reviewQuestions_ = candidate.needsReviewQuestions;
    generatedAssets_ = candidate.assets;
    outputTokens_->setText(QString::number(generatedQuestions_.size()));
    totalTokens_->setText(QString::number(reviewQuestions_.size()));
    reviewTree_->clear();
    QHash<QString, QTreeWidgetItem*> groups;
    for (const auto& value : generatedMaterials_) {
        const QJsonObject material = value.toObject();
        const QString id = material.value("id").toString();
        const QString title = material.value("title").toString(id);
        auto* item = new QTreeWidgetItem(reviewTree_,
            {title, QStringLiteral("共享材料"), material.value("body").toString().left(240)});
        item->setData(0, Qt::UserRole, id);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
        item->setCheckState(0, Qt::Checked);
        groups.insert(id, item);
    }
    auto* independent = new QTreeWidgetItem(reviewTree_,
        {QStringLiteral("独立题目"), QStringLiteral("不引用共享材料"), {}});
    independent->setFlags(independent->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
    independent->setCheckState(0, Qt::Checked);
    QTreeWidgetItem* brokenReferences = nullptr;

    int missingAnswers = 0;
    int duplicates = 0;
    const auto appendQuestions = [&](const QJsonArray& questions, bool needsReview) {
        for (const auto& value : questions) {
            const QJsonObject question = value.toObject();
            const QJsonObject review = question.value("review").toObject();
            const QString reason = review.value("reason").toString();
            if (needsReview && reason.contains(QStringLiteral("答案"))) ++missingAnswers;
            if (needsReview && reason.contains(QStringLiteral("重复"))) ++duplicates;
            const QString materialId = question.value("materialId").toString();
            QTreeWidgetItem* parent = independent;
            if (!materialId.isEmpty()) {
                parent = groups.value(materialId, nullptr);
                if (!parent) {
                    if (!brokenReferences) {
                        brokenReferences = new QTreeWidgetItem(reviewTree_,
                            {QStringLiteral("引用断裂"), QStringLiteral("必须丢弃或修正"), {}});
                        brokenReferences->setFlags(brokenReferences->flags() |
                            Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
                        brokenReferences->setCheckState(0, Qt::Checked);
                    }
                    parent = brokenReferences;
                }
            }
            auto* item = new QTreeWidgetItem(parent,
                {question.value("id").toString(), needsReview
                    ? reason.left(240) : QStringLiteral("已通过规则校验"),
                 question.value("stem").toString().left(300)});
            item->setData(0, Qt::UserRole, question);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            // 规则或模型明确标记为待复核的题目可能缺答案/选项，默认不进入最终包；
            // 用户主动勾选后仍会经过 BankValidator，生产路径绝不补猜测答案。
            item->setCheckState(0, needsReview ? Qt::Unchecked : Qt::Checked);
        }
    };
    appendQuestions(generatedQuestions_, false);
    appendQuestions(reviewQuestions_, true);
    if (independent->childCount() == 0) delete independent;
    reviewTree_->expandToDepth(0);
    allReviewButton_->setText(QStringLiteral("全部异常  %1").arg(reviewQuestions_.size()));
    missingAnswerButton_->setText(QStringLiteral("缺少答案  %1").arg(missingAnswers));
    duplicateButton_->setText(QStringLiteral("疑似重复  %1").arg(duplicates));
}

void StudioWindow::packageProvider() {
    QJsonArray selected;
    QSet<QString> usedMaterialIds;
    for (int topIndex = 0; topIndex < reviewTree_->topLevelItemCount(); ++topIndex) {
        QTreeWidgetItem* group = reviewTree_->topLevelItem(topIndex);
        for (int childIndex = 0; childIndex < group->childCount(); ++childIndex) {
            QTreeWidgetItem* child = group->child(childIndex);
            if (child->checkState(0) == Qt::Unchecked) continue;
            const QJsonObject question = child->data(0, Qt::UserRole).toJsonObject();
            if (question.isEmpty()) continue;
            selected.append(question);
            const QString materialId = question.value("materialId").toString();
            if (!materialId.isEmpty()) usedMaterialIds.insert(materialId);
        }
    }
    if (selected.isEmpty()) {
        diagnostic::event(QStringLiteral("studio"), QStringLiteral("package-rejected"),
            {{QStringLiteral("reason"), QStringLiteral("no-selected-questions")}});
        QMessageBox::warning(this, QStringLiteral("无法生成"), QStringLiteral("至少需要采纳一道题。"));
        return;
    }
    QString title = bankName_->text().trimmed();
    if (title.isEmpty()) title = QStringLiteral("我的题库");
    int questionCount = 0;
    if (questionCount_->currentIndex() < 3)
        questionCount = QList<int>{5, 10, 15}.at(questionCount_->currentIndex());
    QJsonObject practice{{"mode", questionCount == 0 ? "all" : "sequential"}};
    if (questionCount > 0) practice.insert("questionCount", qMin(questionCount, selected.size()));
    QJsonArray selectedMaterials;
    for (const auto& value : generatedMaterials_) {
        const QJsonObject material = value.toObject();
        if (usedMaterialIds.contains(material.value("id").toString()))
            selectedMaterials.append(material);
    }
    QJsonObject bank{{"schemaVersion", 2}, {"title", title},
        {"catalogs", QJsonArray{QJsonObject{{"id", "generated"}, {"title", title},
            {"practice", practice}}}}, {"questions", selected}};
    if (!selectedMaterials.isEmpty()) bank.insert("materials", selectedMaterials);
    QString error;
    if (!quizpane::validateBank(bank, &error)) {
        diagnostic::event(QStringLiteral("studio"), QStringLiteral("package-validation-failed"),
            {{QStringLiteral("questions"), selected.size()},
             {QStringLiteral("materials"), selectedMaterials.size()},
             {QStringLiteral("error"), error}});
        QMessageBox::warning(this, QStringLiteral("题库校验失败"), error);
        return;
    }
    const QString slug = QString::fromLatin1(QCryptographicHash::hash(
        title.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
    const QString version = QStringLiteral("1.0.%1")
        .arg(QDateTime::currentSecsSinceEpoch());
    const QJsonObject manifest{{"manifestVersion", 2}, {"id", "local.generated." + slug},
        {"name", title}, {"version", version}, {"kind", "declarative"},
        {"runtime", QJsonObject{{"format", "quizpane.bank+json"}, {"schemaVersion", 2},
            {"entry", "content/bank.json"}}},
        {"permissions", QJsonObject{{"network", false}}}};
    QTemporaryDir packageDirectory;
    if (!packageDirectory.isValid()) {
        QMessageBox::critical(this, QStringLiteral("无法生成题库"),
                              QStringLiteral("无法创建临时题库目录。"));
        return;
    }
    const QString output = packageDirectory.filePath(QStringLiteral("generated.quizpane-provider"));
    QList<quizpane::ZipFile> packageFiles{
        {QStringLiteral("manifest.json"), QJsonDocument(manifest).toJson(QJsonDocument::Indented)},
        {QStringLiteral("content/bank.json"), QJsonDocument(bank).toJson(QJsonDocument::Indented)}};
    for (auto it = generatedAssets_.cbegin(); it != generatedAssets_.cend(); ++it)
        packageFiles.append({it.key(), it.value()});
    if (!quizpane::writeZipArchive(output, packageFiles, &error)) {
        diagnostic::event(QStringLiteral("studio"), QStringLiteral("package-write-failed"),
            {{QStringLiteral("error"), error}});
        QMessageBox::critical(this, QStringLiteral("打包失败"), error);
        return;
    }
    quizpane::ProviderPackageInfo info;
    quizpane::ProviderInstaller installer;
    if (!installer.inspect(output, &info, &error)) {
        diagnostic::event(QStringLiteral("studio"), QStringLiteral("package-inspect-failed"),
            {{QStringLiteral("error"), error}});
        QFile::remove(output);
        QMessageBox::critical(this, QStringLiteral("安装包自检失败"), error);
        return;
    }
    // 从刚写出的 ZIP 重新读取文件再交给声明式运行时，而不是验证打包前的
    // 内存对象，确保压缩、路径和读取链路本身也进入最终自检。
    quizpane::ZipArchiveReader archive(output);
    const QByteArray packagedManifest = archive.fileData(QStringLiteral("manifest.json"));
    const QByteArray packagedBank = archive.fileData(QStringLiteral("content/bank.json"));
    if (!archive.isReadable() || packagedManifest.isEmpty() || packagedBank.isEmpty()) {
        QFile::remove(output);
        QMessageBox::critical(this, QStringLiteral("最终验证失败"),
                              QStringLiteral("无法从生成的安装包重新读取题库内容"));
        return;
    }
    QTemporaryDir staging;
    QDir().mkpath(staging.filePath(QStringLiteral("content")));
    QFile manifestFile(staging.filePath(QStringLiteral("manifest.json")));
    QFile bankFile(staging.filePath(QStringLiteral("content/bank.json")));
    if (!manifestFile.open(QIODevice::WriteOnly) || !bankFile.open(QIODevice::WriteOnly)) {
        QFile::remove(output);
        QMessageBox::critical(this, QStringLiteral("最终验证失败"), QStringLiteral("无法创建临时验证目录"));
        return;
    }
    manifestFile.write(packagedManifest); manifestFile.close();
    bankFile.write(packagedBank); bankFile.close();
    quizpane::DeclarativeProvider provider;
    if (!provider.load(staging.filePath(QStringLiteral("content/bank.json")), &error)) {
        QFile::remove(output);
        QMessageBox::critical(this, QStringLiteral("最终验证失败"), error);
        return;
    }
    quizpane::ProviderInstallResult installed;
    if (!installer.install(info, &installed, &error)) {
        QMessageBox::critical(this, QStringLiteral("无法添加题库"), error);
        return;
    }
    // 制作器与小窗是独立进程。把已安装题库写到小窗的启动偏好并主动唤起它，
    // 避免 UI 提示“可以刷题”而用户还得从菜单再选一次。
    QSettings practiceSettings(QStringLiteral("QuizPane Project"), QStringLiteral("小窗刷题"));
    practiceSettings.setValue(QStringLiteral("provider/lastLibraryPath"), installed.entryPath);
#if defined(Q_OS_MACOS)
    QProcess::startDetached(QStringLiteral("open"),
                            {QStringLiteral("-a"), QStringLiteral("小窗刷题"),
                             QStringLiteral("--args"), QStringLiteral("--provider"), installed.entryPath});
#endif
    diagnostic::event(QStringLiteral("studio"), QStringLiteral("package-success"),
        {{QStringLiteral("file"), QFileInfo(output).fileName()},
         {QStringLiteral("questions"), selected.size()},
         {QStringLiteral("materials"), selectedMaterials.size()},
         {QStringLiteral("bytes"), QFileInfo(output).size()}});
    finishPath_->setText(QStringLiteral("已添加到小窗刷题：%1\n%2")
        .arg(title, QDir::toNativeSeparators(installed.installDirectory)));
    if (workflow_) workflow_->cancel();
    QMessageBox::information(this, QStringLiteral("题库已添加"),
                             QStringLiteral("“%1”已添加到小窗刷题，可以直接开始练习。").arg(title));
}

// ===== 桌面文件拖放与统一样式 =====

void StudioWindow::dragEnterEvent(QDragEnterEvent* event) {
    for (const QUrl& url : event->mimeData()->urls()) {
        if (!url.isLocalFile() || !acceptedSource(url.toLocalFile())) continue;
        event->acceptProposedAction();
        return;
    }
}

void StudioWindow::dropEvent(QDropEvent* event) {
    QStringList paths;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile()) paths.append(url.toLocalFile());
    }
    appendSources(paths);
    event->acceptProposedAction();
}

void StudioWindow::closeEvent(QCloseEvent* event) {
    if (!workflow_ || !workflow_->isActive()) {
        event->accept();
        return;
    }
    const TaskCloseChoice answer = chooseTaskCloseAction(this);
    if (answer == TaskCloseChoice::Cancel) { event->ignore(); return; }
    if (answer == TaskCloseChoice::Keep) workflow_->pause();
    else workflow_->cancel();
    event->accept();
}

void StudioWindow::applyStyle() {
    QFile style(QStringLiteral(":/styles/studio.qss"));
    if (!style.open(QIODevice::ReadOnly)) {
        qWarning("Unable to load embedded studio stylesheet");
        return;
    }
    setStyleSheet(QString::fromUtf8(style.readAll()));
}

}  // namespace quizpane::studio
