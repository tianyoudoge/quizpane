#include "studio_window.hpp"
#include "quizpane/diagnostic_logger.hpp"

#include "quizpane/bank_validator.hpp"
#include "quizpane/declarative_provider.hpp"
#include "quizpane/provider_installer.hpp"
#include "quizpane/studio/generation_workflow.hpp"
#include "quizpane/zip_archive.hpp"

#include <QComboBox>
#include <QCloseEvent>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDragEnterEvent>
#include <QDir>
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
#include <QListWidget>
#include <QMimeData>
#include <QMenuBar>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedWidget>
#include <QStyle>
#include <QSet>
#include <QTimer>
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

bool acceptedSource(const QString& path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    return QStringList{"txt", "md", "markdown", "docx", "pdf"}.contains(suffix);
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
    connect(generationMode_, &QComboBox::currentIndexChanged, this, [this] {
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
    generationMode_ = new QComboBox;
    generationMode_->addItem(QStringLiteral("离线整理（推荐）"), QStringLiteral("rules"));
    generationMode_->addItem(QStringLiteral("AI 辅助整理"), QStringLiteral("model"));
    modeLayout->addWidget(generationMode_);
    modeLayout->addWidget(mutedLabel(
        QStringLiteral("离线整理不会上传资料，适合题号、选项和答案比较规范的文档；"
                       "AI 辅助整理更适合排版复杂的资料，会使用你配置的模型，结果需要复核。")));
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
    auto* remove = new QPushButton(QStringLiteral("移除所选"));
    remove->setObjectName(QStringLiteral("textButton"));
    auto* addAnswers = new QPushButton(QStringLiteral("添加答案/解析"));
    addAnswers->setObjectName(QStringLiteral("textButton"));
    listHeader->addWidget(listTitle);
    listHeader->addWidget(sourceSummary_);
    listHeader->addStretch();
    listHeader->addWidget(addAnswers);
    listHeader->addWidget(remove);
    sourcePanelLayout->addLayout(listHeader);
    sourceList_ = new QListWidget;
    sourceList_->setObjectName(QStringLiteral("sourceList"));
    sourceList_->setMaximumHeight(210);
    sourcePanelLayout->addWidget(sourceList_);
    sourcePanel_->setVisible(false);
    layout->addWidget(sourcePanel_);
    layout->addStretch();
    connect(remove, &QPushButton::clicked, this, &StudioWindow::removeSelectedSource);
    connect(addAnswers, &QPushButton::clicked, this, &StudioWindow::addAnswerFile);
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
    progressBar_ = new QProgressBar;
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    progressBar_->setTextVisible(false);
    layout->addWidget(phaseLabel_);
    layout->addWidget(phaseDetail_);
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
    questionCount_ = new QComboBox;
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
        auto* item = new QListWidgetItem(QStringLiteral("题目 / 资料  ·  %1\n%2")
            .arg(QFileInfo(absolute).fileName(), QDir::toNativeSeparators(absolute)));
        item->setToolTip(absolute);
        sourceList_->addItem(item);
    }
    diagnostic::event(QStringLiteral("studio"), QStringLiteral("sources-updated"),
        {{QStringLiteral("offered"), paths.size()},
         {QStringLiteral("added"), added},
         {QStringLiteral("total"), sourcePaths_.size()}});
    sourceSummary_->setText(sourcePaths_.isEmpty() ? QStringLiteral("尚未添加文件")
        : QStringLiteral("%1 个文件").arg(sourcePaths_.size()));
    sourcePanel_->setVisible(!sourcePaths_.isEmpty());
    // 新导入的题目就是用户接下来最可能要配对答案的对象；同时避免
    // 空选择让“添加答案/解析”只能弹出一个无助的提示。
    if (added > 0) sourceList_->setCurrentRow(sourceList_->count() - 1);
    updateNavigation();
}

void StudioWindow::addAnswerFile() {
    const int row = sourceList_->currentRow();
    if (row < 0 || row >= sourcePaths_.size()) {
        QMessageBox::information(this, QStringLiteral("先选择题目文件"),
            QStringLiteral("在“已添加资料”中选中一份题目文件后，再添加对应的答案或解析。"));
        return;
    }
    const QString answer = QFileDialog::getOpenFileName(
        this, QStringLiteral("添加答案或解析"), {},
        QStringLiteral("答案或解析 (*.txt *.md *.markdown *.docx *.pdf)"));
    if (answer.isEmpty()) return;
    const QString absolute = QFileInfo(answer).absoluteFilePath();
    if (!acceptedSource(absolute)) return;
    const QString question = sourcePaths_.at(row);
    answerPathsByQuestion_.insert(question, absolute);
    QListWidgetItem* item = sourceList_->item(row);
    item->setText(QStringLiteral("题目  ·  %1\n答案/解析  ·  %2")
        .arg(QFileInfo(question).fileName(), QFileInfo(absolute).fileName()));
    item->setToolTip(question + QStringLiteral("\n") + absolute);
}

void StudioWindow::removeSelectedSource() {
    const int row = sourceList_->currentRow();
    if (row < 0) return;
    answerPathsByQuestion_.remove(sourcePaths_.at(row));
    sourcePaths_.removeAt(row);
    delete sourceList_->takeItem(row);
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
        startButton_->setEnabled(true);
        if (error.contains(QStringLiteral("重新开始"))) {
            const auto answer = QMessageBox::question(this, QStringLiteral("重新开始生成任务？"),
                error + QStringLiteral("\n\n是否删除旧检查点并重新开始？"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (answer == QMessageBox::Yes) {
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
    if (!quizpane::writeZipArchive(output, {
            {QStringLiteral("manifest.json"), QJsonDocument(manifest).toJson(QJsonDocument::Indented)},
            {QStringLiteral("content/bank.json"), QJsonDocument(bank).toJson(QJsonDocument::Indented)}}, &error)) {
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
    const auto answer = QMessageBox::question(this, QStringLiteral("保留生成任务？"),
        QStringLiteral("任务尚未完成。选择“是”会保留检查点，下次添加同一批文件可继续；"
                       "选择“否”会删除检查点。"), QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel,
        QMessageBox::Yes);
    if (answer == QMessageBox::Cancel) { event->ignore(); return; }
    if (answer == QMessageBox::Yes) workflow_->pause();
    else workflow_->cancel();
    event->accept();
}

void StudioWindow::applyStyle() {
    setStyleSheet(R"(
      QMainWindow, QWidget#content { background: #f8fafc; color: #1f2937; }
      QFrame#sidebar { background: #18324d; border: none; }
      QLabel#brand { font-size: 20px; font-weight: 700; color: #ffffff; }
      QLabel#sidebar QLabel#muted, QFrame#sidebar QLabel#privacyFootnote { color: #c7d8e8; }
      QLabel#pageTitle { font-size: 25px; font-weight: 700; color: #16283c; }
      QLabel#eyebrow { color: #4d779d; font-size: 11px; font-weight: 600; }
      QLabel#muted, QLabel#privacyFootnote { color: #64748b; }
      QLabel#privacyFootnote { font-size: 11px; }
      QLabel#sideStep { color: #b7cbde; padding: 9px 10px; border-radius: 6px; }
      QLabel#sideStep[active="true"] { color: #ffffff; background: rgba(255,255,255,28); }
      QLabel#sectionTitle, QLabel#phaseTitle { color: #1f3b57; font-size: 15px; font-weight: 650; }
      QLabel#metricValue { color: #1d4e79; font-size: 21px; font-weight: 700; }
      QLabel#notice { background: #eef6fc; color: #31536e; padding: 12px; border-radius: 8px; }
      QFrame#dropZone { background: #f4f9fd; border: 1px dashed #83a8c7; border-radius: 12px; }
      QFrame#panel, QFrame#metricCard { background: #ffffff; border: 1px solid #e2e8f0; border-radius: 10px; }
      QPushButton { background: #ffffff; color: #334155; border: 1px solid #d8e1ea; border-radius: 7px; padding: 8px 13px; }
      QPushButton:hover { background: #eef6fc; border-color: #8fb4d2; }
      QPushButton#primaryButton { background: #1f6fa8; color: #ffffff; border-color: #1f6fa8; }
      QPushButton#secondaryButton, QPushButton#textButton { background: transparent; color: #2f6e9f; border-color: transparent; }
      QPushButton:disabled { color: #94a3b8; background: #f1f5f9; border-color: #e2e8f0; }
      QLineEdit, QComboBox, QListWidget, QTableWidget, QTreeWidget { background: #ffffff; color: #334155; border: 1px solid #dbe4ec; border-radius: 7px; padding: 8px; selection-background-color: #dceefb; }
      QListWidget::item { color: #1f2937; padding: 10px; border-bottom: 1px solid #edf2f7; }
      QListWidget::item:selected { color: #0f3f63; background: #dceefb; }
      QHeaderView::section { background: #f1f6fa; color: #587087; border: none; padding: 8px; }
      QProgressBar { background: #e6eef5; border: none; border-radius: 3px; height: 7px; }
      QProgressBar::chunk { background: #2e82bb; border-radius: 3px; }
    )");
}

}  // namespace quizpane::studio
