#include "studio_window.hpp"

#include <QComboBox>
#include <QDragEnterEvent>
#include <QDir>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedWidget>
#include <QStyle>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace quizpane::studio {
namespace {

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
    return QStringList{"txt", "md", "docx", "pdf", "json"}.contains(suffix);
}

}  // namespace

StudioWindow::StudioWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("题库生成器 · 小窗刷题"));
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
    auto* brand = new QLabel(QStringLiteral("题库生成器"));
    brand->setObjectName(QStringLiteral("brand"));
    sideLayout->addWidget(brand);
    sideLayout->addWidget(mutedLabel(QStringLiteral("把你的文档整理成可安装题库")));
    sideLayout->addSpacing(22);
    const QStringList steps{QStringLiteral("01  选择资料"), QStringLiteral("02  选择模型"),
        QStringLiteral("03  自动整理"), QStringLiteral("04  检查问题"), QStringLiteral("05  完成")};
    for (int index = 0; index < steps.size(); ++index) {
        auto* step = new QLabel(steps.at(index));
        step->setObjectName(QStringLiteral("sideStep"));
        step->setProperty("stepIndex", index);
        sideLayout->addWidget(step);
    }
    sideLayout->addStretch();
    auto* privacy = mutedLabel(QStringLiteral("原文件默认只在本机读取。调用云端模型前会再次提示上传范围。"));
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
    pages_->addWidget(buildModelPage());
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
    preflightTimer_ = new QTimer(this);
    preflightTimer_->setInterval(380);
    connect(preflightTimer_, &QTimer::timeout, this, &StudioWindow::runPreflightStep);
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

QWidget* StudioWindow::buildSourcePage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(16);
    layout->addWidget(pageHeader(QStringLiteral("第一步"), QStringLiteral("选择你的题目资料"),
        QStringLiteral("支持 TXT、Markdown、DOCX、PDF 和已有 JSON。可以一次添加多个文件。")));
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
    auto* ocrHint = mutedLabel(QStringLiteral("扫描版 PDF 会在后续提示是否启用 OCR"));
    ocrHint->setAlignment(Qt::AlignCenter);
    dropLayout->addWidget(ocrHint);
    dropLayout->addWidget(addButton, 0, Qt::AlignHCenter);
    layout->addWidget(drop);
    connect(addButton, &QPushButton::clicked, this, &StudioWindow::addSourceFiles);

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
    layout->addLayout(listHeader);
    sourceList_ = new QListWidget;
    sourceList_->setObjectName(QStringLiteral("sourceList"));
    layout->addWidget(sourceList_, 1);
    connect(remove, &QPushButton::clicked, this, &StudioWindow::removeSelectedSource);
    return page;
}

QWidget* StudioWindow::buildModelPage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(16);
    layout->addWidget(pageHeader(QStringLiteral("第二步"), QStringLiteral("选择整理题目的模型"),
        QStringLiteral("推荐使用支持结构化输出的模型。连接功能完成后，API Key 将只保存在系统安全凭据库。")));
    auto* form = new QFrame;
    form->setObjectName(QStringLiteral("panel"));
    auto* formLayout = new QVBoxLayout(form);
    formLayout->setContentsMargins(20, 18, 20, 18);
    formLayout->setSpacing(10);
    formLayout->addWidget(new QLabel(QStringLiteral("模型服务")));
    modelProvider_ = new QComboBox;
    modelProvider_->addItems({QStringLiteral("OpenAI"), QStringLiteral("OpenAI 兼容服务"),
                              QStringLiteral("Ollama 本地模型")});
    formLayout->addWidget(modelProvider_);
    formLayout->addWidget(new QLabel(QStringLiteral("模型")));
    modelName_ = new QComboBox;
    modelName_->setEditable(true);
    formLayout->addWidget(modelName_);
    formLayout->addWidget(new QLabel(QStringLiteral("服务地址")));
    endpointEdit_ = new QLineEdit;
    formLayout->addWidget(endpointEdit_);
    formLayout->addWidget(new QLabel(QStringLiteral("API Key")));
    apiKeyEdit_ = new QLineEdit;
    apiKeyEdit_->setEchoMode(QLineEdit::Password);
    apiKeyEdit_->setPlaceholderText(QStringLiteral("本地 Ollama 不需要填写"));
    formLayout->addWidget(apiKeyEdit_);
    auto* test = new QPushButton(QStringLiteral("测试连接"));
    test->setObjectName(QStringLiteral("secondaryButton"));
    formLayout->addWidget(test, 0, Qt::AlignLeft);
    layout->addWidget(form);
    privacyHint_ = mutedLabel(QString());
    privacyHint_->setObjectName(QStringLiteral("notice"));
    layout->addWidget(privacyHint_);
    layout->addStretch();
    connect(modelProvider_, &QComboBox::currentIndexChanged, this, &StudioWindow::updateModelFields);
    connect(test, &QPushButton::clicked, this, [this] {
        privacyHint_->setText(QStringLiteral("连接测试将在模型适配器接入后启用；当前不会发送文件或 API Key。"));
    });
    updateModelFields();
    return page;
}

QWidget* StudioWindow::buildProgressPage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(18);
    layout->addWidget(pageHeader(QStringLiteral("第三步"), QStringLiteral("自动整理题目"),
        QStringLiteral("可以随时看到处理阶段和 Token 用量；关闭窗口前会先询问是否保存任务。")));
    phaseLabel_ = new QLabel(QStringLiteral("等待开始"));
    phaseLabel_->setObjectName(QStringLiteral("phaseTitle"));
    phaseDetail_ = mutedLabel(QStringLiteral("点击下方“开始整理”后，先在本地检查资料。"));
    progressBar_ = new QProgressBar;
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    progressBar_->setTextVisible(false);
    layout->addWidget(phaseLabel_);
    layout->addWidget(phaseDetail_);
    layout->addWidget(progressBar_);
    auto* metrics = new QHBoxLayout;
    metrics->addWidget(metricCard(QStringLiteral("输入 Token（估算）"), &inputTokens_));
    metrics->addWidget(metricCard(QStringLiteral("输出 Token"), &outputTokens_));
    metrics->addWidget(metricCard(QStringLiteral("本次合计"), &totalTokens_));
    layout->addLayout(metrics);
    auto* stages = new QFrame;
    stages->setObjectName(QStringLiteral("panel"));
    auto* stagesLayout = new QVBoxLayout(stages);
    stagesLayout->setContentsMargins(18, 16, 18, 16);
    stagesLayout->addWidget(new QLabel(QStringLiteral("处理阶段")));
    stagesLayout->addWidget(mutedLabel(QStringLiteral("读取文件  →  提取文字与图片  →  分段  →  模型整理  →  规则校验  →  修复与去重")));
    layout->addWidget(stages);
    layout->addStretch();
    return page;
}

QWidget* StudioWindow::buildReviewPage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(16);
    layout->addWidget(pageHeader(QStringLiteral("第四步"), QStringLiteral("只检查需要你决定的问题"),
        QStringLiteral("正常题目会自动收起。这里集中展示缺答案、疑似重复和选项错位。")));
    auto* filters = new QHBoxLayout;
    filters->addWidget(new QPushButton(QStringLiteral("全部异常  0")));
    filters->addWidget(new QPushButton(QStringLiteral("缺少答案  0")));
    filters->addWidget(new QPushButton(QStringLiteral("疑似重复  0")));
    filters->addStretch();
    layout->addLayout(filters);
    reviewTable_ = new QTableWidget(0, 4);
    reviewTable_->setHorizontalHeaderLabels({QStringLiteral("位置"), QStringLiteral("问题"),
                                              QStringLiteral("题目预览"), QStringLiteral("处理")});
    reviewTable_->horizontalHeader()->setStretchLastSection(true);
    reviewTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    reviewTable_->verticalHeader()->hide();
    reviewTable_->setAlternatingRowColors(false);
    layout->addWidget(reviewTable_, 1);
    return page;
}

QWidget* StudioWindow::buildFinishPage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(16);
    layout->addWidget(pageHeader(QStringLiteral("第五步"), QStringLiteral("生成并安装题库"),
        QStringLiteral("确认题库名称和组卷方式，生成后可以直接交给小窗刷题安装。")));
    auto* panel = new QFrame;
    panel->setObjectName(QStringLiteral("panel"));
    auto* form = new QVBoxLayout(panel);
    form->setContentsMargins(20, 18, 20, 18);
    form->addWidget(new QLabel(QStringLiteral("题库名称")));
    auto* name = new QLineEdit;
    name->setPlaceholderText(QStringLiteral("例如：我的行测常识题库"));
    form->addWidget(name);
    form->addWidget(new QLabel(QStringLiteral("默认每套题数量")));
    auto* count = new QComboBox;
    count->addItems({QStringLiteral("5 题"), QStringLiteral("10 题"), QStringLiteral("15 题"), QStringLiteral("全部题目")});
    form->addWidget(count);
    finishPath_ = mutedLabel(QStringLiteral("输出格式：跨平台 .quizpane-provider"));
    form->addWidget(finishPath_);
    layout->addWidget(panel);
    layout->addStretch();
    return page;
}

void StudioWindow::addSourceFiles() {
    appendSources(QFileDialog::getOpenFileNames(this, QStringLiteral("选择题目资料"), {},
        QStringLiteral("题目资料 (*.txt *.md *.docx *.pdf *.json)")));
}

void StudioWindow::appendSources(const QStringList& paths) {
    for (const QString& path : paths) {
        const QString absolute = QFileInfo(path).absoluteFilePath();
        if (!acceptedSource(absolute) || sourcePaths_.contains(absolute)) continue;
        sourcePaths_.append(absolute);
        auto* item = new QListWidgetItem(QStringLiteral("%1\n%2")
            .arg(QFileInfo(absolute).fileName(), QDir::toNativeSeparators(absolute)));
        item->setToolTip(absolute);
        sourceList_->addItem(item);
    }
    sourceSummary_->setText(sourcePaths_.isEmpty() ? QStringLiteral("尚未添加文件")
        : QStringLiteral("%1 个文件").arg(sourcePaths_.size()));
    updateNavigation();
}

void StudioWindow::removeSelectedSource() {
    const int row = sourceList_->currentRow();
    if (row < 0) return;
    sourcePaths_.removeAt(row);
    delete sourceList_->takeItem(row);
    sourceSummary_->setText(sourcePaths_.isEmpty() ? QStringLiteral("尚未添加文件")
        : QStringLiteral("%1 个文件").arg(sourcePaths_.size()));
    updateNavigation();
}

void StudioWindow::updateModelFields() {
    modelName_->clear();
    if (modelProvider_->currentIndex() == 0) {
        modelName_->addItems({QStringLiteral("自动选择（推荐）"), QStringLiteral("填写模型名称…")});
        endpointEdit_->setText(QStringLiteral("https://api.openai.com/v1"));
        apiKeyEdit_->setEnabled(true);
        privacyHint_->setText(QStringLiteral("云端模式：只发送分段后的题目文本；开始前会显示预计上传量。"));
    } else if (modelProvider_->currentIndex() == 1) {
        modelName_->addItem(QStringLiteral("填写模型名称"));
        endpointEdit_->setText(QStringLiteral("https://your-endpoint.example/v1"));
        apiKeyEdit_->setEnabled(true);
        privacyHint_->setText(QStringLiteral("兼容服务由用户自行选择，请确认其隐私与数据保留政策。"));
    } else {
        modelName_->addItems({QStringLiteral("qwen3:8b"), QStringLiteral("qwen3:4b")});
        endpointEdit_->setText(QStringLiteral("http://127.0.0.1:11434/v1"));
        apiKeyEdit_->clear(); apiKeyEdit_->setEnabled(false);
        privacyHint_->setText(QStringLiteral("本地模式：资料不离开电脑，但速度取决于本机性能。"));
    }
}

void StudioWindow::movePage(int delta) {
    pages_->setCurrentIndex(qBound(0, pages_->currentIndex() + delta, pages_->count() - 1));
}

void StudioWindow::updateNavigation() {
    const int page = pages_->currentIndex();
    backButton_->setVisible(page > 0 && page != 2);
    nextButton_->setVisible(page < 2 || page == 3);
    startButton_->setVisible(page == 2 || page == 4);
    nextButton_->setEnabled(page != 0 || !sourcePaths_.isEmpty());
    startButton_->setText(page == 4 ? QStringLiteral("生成题库安装包") : QStringLiteral("开始整理"));
    const auto steps = findChildren<QLabel*>(QStringLiteral("sideStep"));
    for (QLabel* step : steps) {
        step->setProperty("active", step->property("stepIndex").toInt() == page);
        step->style()->unpolish(step); step->style()->polish(step);
    }
}

void StudioWindow::beginPreflight() {
    if (pages_->currentIndex() == 4) {
        finishPath_->setText(QStringLiteral("生成工作流将在模型与复核模块接入后启用。"));
        return;
    }
    if (sourcePaths_.isEmpty()) return;
    preflightStep_ = 0; estimatedInputTokens_ = 0;
    progressBar_->setValue(4);
    phaseLabel_->setText(QStringLiteral("正在检查资料"));
    phaseDetail_->setText(QStringLiteral("读取文件大小、格式和本地可提取内容…"));
    startButton_->setEnabled(false);
    preflightTimer_->start();
}

void StudioWindow::runPreflightStep() {
    if (preflightStep_ < sourcePaths_.size()) {
        const QFileInfo file(sourcePaths_.at(preflightStep_));
        estimatedInputTokens_ += qMax<qint64>(1, file.size() / 4);
        ++preflightStep_;
        progressBar_->setValue(5 + 25 * preflightStep_ / sourcePaths_.size());
        inputTokens_->setText(QString::number(estimatedInputTokens_));
        totalTokens_->setText(QString::number(estimatedInputTokens_));
        phaseDetail_->setText(QStringLiteral("已检查 %1 / %2：%3")
            .arg(preflightStep_).arg(sourcePaths_.size()).arg(file.fileName()));
        return;
    }
    preflightTimer_->stop();
    progressBar_->setValue(30);
    phaseLabel_->setText(QStringLiteral("本地预检完成"));
    phaseDetail_->setText(QStringLiteral("模型调用尚未接入，因此没有发送资料，也没有产生输出 Token。后续可从此检查点继续。"));
    startButton_->setText(QStringLiteral("等待模型适配器"));
}

void StudioWindow::dragEnterEvent(QDragEnterEvent* event) {
    for (const QUrl& url : event->mimeData()->urls())
        if (url.isLocalFile() && acceptedSource(url.toLocalFile())) { event->acceptProposedAction(); return; }
}

void StudioWindow::dropEvent(QDropEvent* event) {
    QStringList paths;
    for (const QUrl& url : event->mimeData()->urls()) if (url.isLocalFile()) paths.append(url.toLocalFile());
    appendSources(paths); event->acceptProposedAction();
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
      QLineEdit, QComboBox, QListWidget, QTableWidget { background: #1d2127; color: #cbd0d6; border: none; border-radius: 6px; padding: 8px; selection-background-color: #353c45; }
      QListWidget::item { padding: 9px; border-bottom: 1px solid rgba(255,255,255,7); }
      QHeaderView::section { background: #20242a; color: #929ba5; border: none; padding: 8px; }
      QProgressBar { background: #242931; border: none; border-radius: 3px; height: 6px; }
      QProgressBar::chunk { background: #7e8996; border-radius: 3px; }
    )");
}

}  // namespace quizpane::studio
