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
                QStringLiteral("当前方式：规则结构化 · 完全离线，不消耗 Token"));
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
        QStringLiteral("第一步"), QStringLiteral("选择你的题目资料"),
        QStringLiteral("支持 TXT、Markdown、DOCX 和 PDF；规整文档可完全离线整理。")));
    auto* modePanel = new QFrame;
    modePanel->setObjectName(QStringLiteral("panel"));
    auto* modeLayout = new QVBoxLayout(modePanel);
    modeLayout->setContentsMargins(16, 12, 16, 12);
    modeLayout->addWidget(new QLabel(QStringLiteral("整理方式")));
    generationMode_ = new QComboBox;
    generationMode_->addItem(QStringLiteral("规则结构化（离线、快速）"), QStringLiteral("rules"));
    generationMode_->addItem(QStringLiteral("模型生成（原有方式）"), QStringLiteral("model"));
    modeLayout->addWidget(generationMode_);
    modeLayout->addWidget(mutedLabel(
        QStringLiteral("规则模式不会上传资料；无法可靠识别的题目会进入复核列表。")));
    layout->addWidget(modePanel);
    auto* drop = new QFrame;
    drop->setObjectName(QStringLiteral("dropZone"));
    auto* dropLayout = new QVBoxLayout(drop);
    dropLayout->setContentsMargins(22, 24, 22, 24);
    auto* dropTitle = new QLabel(QStringLiteral("拖入文件，或从电脑中选择"));
    dropTitle->setObjectName(QStringLiteral("sectionTitle"));
    dropTitle->setAlignment(Qt::AlignCenter);
    auto* addButton = new QPushButton(QStringLiteral("选择资料"));
    addButton->setObjectName(QStringLiteral("primaryButton"));
    addButton->setFixedWidth(120);
    dropLayout->addWidget(dropTitle);
    auto* ocrHint = mutedLabel(QStringLiteral(
        "扫描版 PDF 会使用发行包内置的 Tesseract C++ OCR 组件"));
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
    listHeader->addWidget(listTitle);
    listHeader->addWidget(sourceSummary_);
    listHeader->addStretch();
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
    return page;
}

QWidget* StudioWindow::buildProgressPage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(18);
    layout->addWidget(pageHeader(
        QStringLiteral("第二步"), QStringLiteral("自动整理题目"),
        QStringLiteral("可以随时看到处理阶段和 Token 用量；关闭窗口前会先询问是否保存任务。")));
    modelSummary_ = mutedLabel(
        QStringLiteral("当前方式：规则结构化 · 完全离线，不消耗 Token"));
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
    metrics->addWidget(metricCard(QStringLiteral("输入 Token（模型返回）"), &inputTokens_));
    metrics->addWidget(metricCard(QStringLiteral("输出 Token"), &outputTokens_));
    metrics->addWidget(metricCard(QStringLiteral("本次合计"), &totalTokens_));
    layout->addLayout(metrics);
    auto* stages = new QFrame;
    stages->setObjectName(QStringLiteral("panel"));
    auto* stagesLayout = new QVBoxLayout(stages);
    stagesLayout->setContentsMargins(18, 16, 18, 16);
    stagesLayout->addWidget(new QLabel(QStringLiteral("处理阶段")));
    stagesLayout->addWidget(mutedLabel(
        QStringLiteral("读取文件  →  DOCX/PDF/OCR 提取  →  题号切分  →  "
                       "选项/答案/解析匹配  →  规则校验")));
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
        QStringLiteral("第四步"), QStringLiteral("生成并安装题库"),
        QStringLiteral("确认题库名称和组卷方式，生成后可以直接交给小窗刷题安装。")));
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
    finishPath_ = mutedLabel(QStringLiteral("输出格式：跨平台 .quizpane-provider"));
    form->addWidget(finishPath_);
    layout->addWidget(panel);
    layout->addStretch();
    return page;
}

// ===== 资料列表与模型设置 =====

void StudioWindow::addSourceFiles() {
    appendSources(QFileDialog::getOpenFileNames(this, QStringLiteral("选择题目资料"), {},
        QStringLiteral("题目资料 (*.txt *.md *.markdown *.docx *.pdf)")));
}

void StudioWindow::appendSources(const QStringList& paths) {
    int added = 0;
    for (const QString& path : paths) {
        const QString absolute = QFileInfo(path).absoluteFilePath();
        if (!acceptedSource(absolute) || sourcePaths_.contains(absolute)) continue;
        sourcePaths_.append(absolute);
        ++added;
        auto* item = new QListWidgetItem(QStringLiteral("%1\n%2")
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
    updateNavigation();
}

void StudioWindow::removeSelectedSource() {
    const int row = sourceList_->currentRow();
    if (row < 0) return;
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
    inputTokens_->setText(QStringLiteral("0"));
    outputTokens_->setText(QStringLiteral("0"));
    totalTokens_->setText(QStringLiteral("0"));
    startButton_->setEnabled(false);
    if (ruleBased) workflow_->startRuleBased(sourcePaths_);
    else workflow_->start(sourcePaths_, modelSettings_);
}

void StudioWindow::updateWorkflowProgress(const WorkflowProgress& progress) {
    int base = 0, span = 0;
    QString phase;
    switch (progress.stage) {
    case WorkflowStage::Extracting: base = 5; span = 20; phase = QStringLiteral("提取文字"); break;
    case WorkflowStage::Chunking: base = 25; span = 5; phase = QStringLiteral("文档分段"); break;
    case WorkflowStage::Generating: base = 30; span = 40; phase = QStringLiteral("模型结构化"); break;
    case WorkflowStage::Validating: base = 70; span = 15; phase = QStringLiteral("规则校验"); break;
    case WorkflowStage::Repairing: base = 85; span = 10; phase = QStringLiteral("定向修复"); break;
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
    inputTokens_->setText(QString::number(progress.inputTokens));
    outputTokens_->setText(QString::number(progress.outputTokens));
    totalTokens_->setText(QString::number(progress.inputTokens + progress.outputTokens));
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
    const QJsonObject manifest{{"manifestVersion", 2}, {"id", "local.generated." + slug},
        {"name", title}, {"version", "1.0.0"}, {"kind", "declarative"},
        {"runtime", QJsonObject{{"format", "quizpane.bank+json"}, {"schemaVersion", 2},
            {"entry", "content/bank.json"}}},
        {"permissions", QJsonObject{{"network", false}}}};
    QString output = QFileDialog::getSaveFileName(this, QStringLiteral("保存题库安装包"),
        title + QStringLiteral(".quizpane-provider"), QStringLiteral("QuizPane 题库 (*.quizpane-provider)"));
    if (output.isEmpty()) return;
    if (!output.endsWith(QStringLiteral(".quizpane-provider"), Qt::CaseInsensitive))
        output += QStringLiteral(".quizpane-provider");
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
    diagnostic::event(QStringLiteral("studio"), QStringLiteral("package-success"),
        {{QStringLiteral("file"), QFileInfo(output).fileName()},
         {QStringLiteral("questions"), selected.size()},
         {QStringLiteral("materials"), selectedMaterials.size()},
         {QStringLiteral("bytes"), QFileInfo(output).size()}});
    finishPath_->setText(QStringLiteral("已生成并通过安装/运行自检：%1").arg(QDir::toNativeSeparators(output)));
    if (workflow_) workflow_->cancel();
    QMessageBox::information(this, QStringLiteral("生成完成"), finishPath_->text());
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
      QMainWindow, QWidget#content { background: #171a1f; color: #cbd0d6; }
      QFrame#sidebar { background: #111419; border: none; }
      QLabel#brand { font-size: 19px; font-weight: 650; color: #e0e3e7; }
      QLabel#pageTitle { font-size: 25px; font-weight: 650; color: #e1e4e8; }
      QLabel#eyebrow { color: #89929c; font-size: 11px; }
      QLabel#muted, QLabel#privacyFootnote { color: #8f98a3; }
      QLabel#privacyFootnote { font-size: 11px; }
      QLabel#sideStep { color: #737c86; padding: 9px 10px; border-radius: 6px; }
      QLabel#sideStep[active="true"] { color: #d5d9de; background: rgba(255,255,255,10); }
      QLabel#sectionTitle, QLabel#phaseTitle { color: #d6dae0; font-size: 15px; font-weight: 600; }
      QLabel#metricValue { color: #d9dde2; font-size: 21px; font-weight: 600; }
      QLabel#notice { background: rgba(255,255,255,7); padding: 12px; border-radius: 7px; }
      QFrame#dropZone { background: rgba(255,255,255,5); border: 1px dashed #3a414a; border-radius: 10px; }
      QFrame#panel, QFrame#metricCard { background: rgba(255,255,255,6); border: none; border-radius: 9px; }
      QPushButton { background: rgba(255,255,255,10); color: #bac1c9; border: none; border-radius: 6px; padding: 8px 13px; }
      QPushButton:hover { background: rgba(255,255,255,17); }
      QPushButton#primaryButton { background: #343a42; color: #e2e5e9; }
      QPushButton#secondaryButton, QPushButton#textButton { background: transparent; color: #9da5ae; }
      QPushButton:disabled { color: #626a73; background: rgba(255,255,255,4); }
      QLineEdit, QComboBox, QListWidget, QTableWidget, QTreeWidget { background: #1d2127; color: #cbd0d6; border: none; border-radius: 6px; padding: 8px; selection-background-color: #353c45; }
      QListWidget::item { padding: 9px; border-bottom: 1px solid rgba(255,255,255,7); }
      QHeaderView::section { background: #20242a; color: #929ba5; border: none; padding: 8px; }
      QProgressBar { background: #242931; border: none; border-radius: 3px; height: 6px; }
      QProgressBar::chunk { background: #7e8996; border-radius: 3px; }
    )");
}

}  // namespace quizpane::studio
