#include "studio_window.hpp"

#include <QComboBox>
#include <QColor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDir>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QMenuBar>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QStackedWidget>
#include <QStyle>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>

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

struct ModelVendor {
    QString id;
    QString name;
    QString badge;
    QColor badgeColor;
    QString endpoint;
    QString accountUrl;
    QString tutorialUrl;
    QStringList fallbackModels;
    bool local = false;
    bool custom = false;
    bool anthropicProtocol = false;
};

const QList<ModelVendor>& modelVendors() {
    // Endpoint 属于应用内置兼容契约。普通用户只能选择厂商，不能误改地址；
    // 真正需要代理、网关或私有部署的高级用户使用“自定义供应商”。
    static const QList<ModelVendor> vendors{
        {QStringLiteral("openai"), QStringLiteral("OpenAI"), QStringLiteral("AI"),
         QColor(QStringLiteral("#2f6f61")), QStringLiteral("https://api.openai.com/v1"),
         QStringLiteral("https://platform.openai.com/api-keys"),
         QStringLiteral("https://platform.openai.com/docs/quickstart"),
         {QStringLiteral("gpt-5.2"), QStringLiteral("gpt-5-mini"),
          QStringLiteral("gpt-4.1-mini")}},
        {QStringLiteral("anthropic"), QStringLiteral("Anthropic Claude"), QStringLiteral("C"),
         QColor(QStringLiteral("#8b6248")), QStringLiteral("https://api.anthropic.com/v1"),
         QStringLiteral("https://console.anthropic.com/settings/keys"),
         QStringLiteral("https://docs.anthropic.com/en/api/getting-started"),
         {QStringLiteral("claude-sonnet-4-5"), QStringLiteral("claude-haiku-4-5")},
         false, false, true},
        {QStringLiteral("deepseek"), QStringLiteral("DeepSeek"), QStringLiteral("DS"),
         QColor(QStringLiteral("#355d91")), QStringLiteral("https://api.deepseek.com/v1"),
         QStringLiteral("https://platform.deepseek.com/api_keys"),
         QStringLiteral("https://api-docs.deepseek.com/"),
         {QStringLiteral("deepseek-v4-flash"), QStringLiteral("deepseek-v4-pro")}},
        {QStringLiteral("dashscope"), QStringLiteral("阿里云百炼 · 通义千问"), QStringLiteral("Q"),
         QColor(QStringLiteral("#6a55a6")),
         QStringLiteral("https://dashscope.aliyuncs.com/compatible-mode/v1"),
         QStringLiteral("https://bailian.console.aliyun.com/?apiKey=1"),
         QStringLiteral("https://help.aliyun.com/zh/model-studio/get-api-key"),
         {QStringLiteral("qwen3.7-plus"), QStringLiteral("qwen3.6-flash"),
          QStringLiteral("qwen-plus")}},
        {QStringLiteral("zhipu"), QStringLiteral("智谱 AI"), QStringLiteral("GLM"),
         QColor(QStringLiteral("#3c6c8c")),
         QStringLiteral("https://open.bigmodel.cn/api/paas/v4"),
         QStringLiteral("https://open.bigmodel.cn/usercenter/apikeys"),
         QStringLiteral("https://docs.bigmodel.cn/cn/guide/develop/http/introduction"),
         {QStringLiteral("glm-5.1"), QStringLiteral("glm-5-turbo"),
          QStringLiteral("glm-4.7")}},
        {QStringLiteral("ollama"), QStringLiteral("Ollama 本地模型"), QStringLiteral("O"),
         QColor(QStringLiteral("#4e5964")), QStringLiteral("http://127.0.0.1:11434/v1"),
         QStringLiteral("https://ollama.com/download"),
         QStringLiteral("https://docs.ollama.com/api/introduction"),
         {QStringLiteral("qwen3:8b"), QStringLiteral("qwen3:4b"),
          QStringLiteral("llama3.2:3b")}, true},
        {QStringLiteral("custom"), QStringLiteral("自定义供应商（高级）"), QStringLiteral("+"),
         QColor(QStringLiteral("#59616a")), QString(), QString(),
         QStringLiteral("https://github.com/tianyoudoge/quizpane/blob/master/docs/题库生成器UI与集成方案.md"),
         {QStringLiteral("填写模型名称")}, false, true}
    };
    return vendors;
}

const ModelVendor& vendorById(const QString& id) {
    for (const auto& vendor : modelVendors())
        if (vendor.id == id) return vendor;
    return modelVendors().first();
}

QIcon vendorIcon(const ModelVendor& vendor) {
    QPixmap pixmap(28, 28);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(vendor.badgeColor);
    painter.drawRoundedRect(QRectF(2, 2, 24, 24), 6, 6);
    QFont font = painter.font();
    font.setBold(true);
    font.setPixelSize(vendor.badge.size() > 2 ? 8 : 11);
    painter.setFont(font);
    painter.setPen(QColor(QStringLiteral("#f0f2f4")));
    painter.drawText(pixmap.rect(), Qt::AlignCenter, vendor.badge);
    return QIcon(pixmap);
}

QString modelsEndpoint(const ModelVendor& vendor, const QString& endpoint) {
    if (vendor.local) return QStringLiteral("http://127.0.0.1:11434/api/tags");
    QString base = endpoint.trimmed();
    while (base.endsWith('/')) base.chop(1);
    return base + QStringLiteral("/models");
}

QStringList parseModelNames(const QByteArray& payload, bool ollama) {
    const QJsonDocument document = QJsonDocument::fromJson(payload);
    if (!document.isObject()) return {};
    const QJsonArray values = document.object()
        .value(ollama ? QStringLiteral("models") : QStringLiteral("data")).toArray();
    QSet<QString> unique;
    for (const QJsonValue& value : values) {
        const QJsonObject object = value.toObject();
        const QString name = object.value(ollama ? QStringLiteral("name")
                                                 : QStringLiteral("id")).toString().trimmed();
        if (!name.isEmpty()) unique.insert(name);
    }
    QStringList names;
    for (const QString& name : unique) names.append(name);
    names.sort(Qt::CaseInsensitive);
    return names;
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
    const QStringList steps{QStringLiteral("01  选择资料"), QStringLiteral("02  自动整理"),
        QStringLiteral("03  检查问题"), QStringLiteral("04  完成")};
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
    auto* settingsMenu = menuBar()->addMenu(QStringLiteral("设置"));
    auto* modelSettingsAction = settingsMenu->addAction(
        QStringLiteral("模型设置…"), this, &StudioWindow::showModelSettings);
    modelSettingsAction->setShortcut(QKeySequence::Preferences);
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
    layout->addWidget(pageHeader(QStringLiteral("第二步"), QStringLiteral("自动整理题目"),
        QStringLiteral("可以随时看到处理阶段和 Token 用量；关闭窗口前会先询问是否保存任务。")));
    modelSummary_ = mutedLabel(QStringLiteral("当前模型：%1 · %2（可在“设置 → 模型设置”中修改）")
        .arg(modelService_, modelName_));
    modelSummary_->setObjectName(QStringLiteral("notice"));
    layout->addWidget(modelSummary_);
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
    layout->addWidget(pageHeader(QStringLiteral("第三步"), QStringLiteral("只检查需要你决定的问题"),
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
    layout->addWidget(pageHeader(QStringLiteral("第四步"), QStringLiteral("生成并安装题库"),
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
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("模型设置"));
    dialog.setMinimumWidth(660);
    auto* layout = new QVBoxLayout(&dialog);
    auto* title = new QLabel(QStringLiteral("模型设置"));
    title->setObjectName(QStringLiteral("pageTitle"));
    layout->addWidget(title);
    layout->addWidget(mutedLabel(QStringLiteral(
        "选择厂商并填写 API Key，生成器会优先获取账号当前可用模型；网络失败时自动使用内置推荐列表。")));

    auto* form = new QFormLayout;
    form->setSpacing(10);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto* service = new QComboBox;
    for (const auto& vendor : modelVendors())
        service->addItem(vendorIcon(vendor), vendor.name, vendor.id);
    const int savedVendor = service->findData(modelVendorId_);
    service->setCurrentIndex(savedVendor < 0 ? 0 : savedVendor);
    auto* model = new QComboBox;
    model->setEditable(true);
    auto* endpoint = new QLineEdit(modelEndpoint_);
    endpoint->setCursorPosition(0);
    auto* apiKey = new QLineEdit;
    apiKey->setEchoMode(QLineEdit::Password);
    apiKey->setText(modelApiKey_);
    apiKey->setClearButtonEnabled(true);
    apiKey->setPlaceholderText(QStringLiteral("粘贴 API Key"));
    auto* links = new QLabel;
    links->setOpenExternalLinks(true);
    links->setTextInteractionFlags(Qt::TextBrowserInteraction);
    auto* hint = mutedLabel(QString());
    hint->setObjectName(QStringLiteral("notice"));
    form->addRow(QStringLiteral("模型厂商"), service);
    form->addRow(QStringLiteral("模型"), model);
    form->addRow(QStringLiteral("Endpoint"), endpoint);
    form->addRow(QStringLiteral("API Key"), apiKey);
    form->addRow(QStringLiteral("帮助"), links);
    layout->addLayout(form);
    layout->addWidget(hint);

    auto* manager = new QNetworkAccessManager(&dialog);
    QNetworkReply* currentReply = nullptr;
    const auto selectedVendor = [service]() -> const ModelVendor& {
        return vendorById(service->currentData().toString());
    };
    const auto loadModels = [model](const QStringList& names, const QString& preferred) {
        model->clear();
        model->addItems(names);
        if (!preferred.isEmpty() && model->findText(preferred) < 0)
            model->insertItem(0, preferred);
        if (!preferred.isEmpty()) model->setCurrentText(preferred);
        else if (!names.isEmpty()) model->setCurrentIndex(0);
    };
    const auto loadFallback = [model, hint, loadModels](const ModelVendor& vendor,
                                                        const QString& reason,
                                                        const QString& preferred = QString()) {
        loadModels(vendor.fallbackModels, preferred);
        hint->setText(reason.isEmpty()
            ? QStringLiteral("已加载内置推荐模型。填写 API Key 后可获取账号当前可用列表。")
            : QStringLiteral("%1；已回退到内置推荐模型。").arg(reason));
    };

    auto* fetch = new QPushButton(QStringLiteral("获取最新模型"));
    fetch->setObjectName(QStringLiteral("secondaryButton"));
    layout->addWidget(fetch, 0, Qt::AlignLeft);

    std::function<void()> fetchModels;
    fetchModels = [&, service, model, endpoint, apiKey, hint, fetch, manager,
                   selectedVendor, loadFallback, loadModels] {
        const ModelVendor vendor = selectedVendor();
        if (currentReply) {
            currentReply->abort();
            currentReply->deleteLater();
            currentReply = nullptr;
        }
        if (!vendor.local && apiKey->text().trimmed().isEmpty()) {
            loadFallback(vendor, QStringLiteral("请先填写 API Key"), model->currentText());
            return;
        }
        const QUrl url(modelsEndpoint(vendor, endpoint->text()));
        if (!url.isValid() || url.scheme().isEmpty() || url.host().isEmpty()) {
            loadFallback(vendor, QStringLiteral("Endpoint 格式不正确"), model->currentText());
            return;
        }
        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::UserAgentHeader,
                          QStringLiteral("QuizPane-Bank-Studio"));
        request.setRawHeader("Accept", "application/json");
        request.setTransferTimeout(12000);
        const QByteArray key = apiKey->text().trimmed().toUtf8();
        if (vendor.anthropicProtocol) {
            request.setRawHeader("x-api-key", key);
            request.setRawHeader("anthropic-version", "2023-06-01");
        } else if (!vendor.local) {
            request.setRawHeader("Authorization", "Bearer " + key);
        }
        fetch->setEnabled(false);
        fetch->setText(QStringLiteral("正在获取…"));
        hint->setText(QStringLiteral("正在从 %1 获取可用模型，不会上传题目资料…")
                          .arg(vendor.name));
        currentReply = manager->get(request);
        QNetworkReply* reply = currentReply;
        connect(reply, &QNetworkReply::finished, &dialog,
                [&, reply, vendor, model, hint, fetch, loadFallback, loadModels] {
            if (currentReply == reply) currentReply = nullptr;
            fetch->setEnabled(true);
            fetch->setText(QStringLiteral("获取最新模型"));
            const int status = reply->attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray payload = reply->readAll();
            const QString preferred = model->currentText();
            if (reply->error() != QNetworkReply::NoError || status >= 400) {
                const QString reason = status == 401 || status == 403
                    ? QStringLiteral("API Key 无效或没有模型列表权限")
                    : QStringLiteral("获取失败：%1").arg(reply->errorString());
                loadFallback(vendor, reason, preferred);
            } else {
                const QStringList names = parseModelNames(payload, vendor.local);
                if (names.isEmpty()) {
                    loadFallback(vendor, QStringLiteral("厂商没有返回可识别的模型列表"), preferred);
                } else {
                    loadModels(names, names.contains(preferred) ? preferred : QString());
                    hint->setText(QStringLiteral("已从 %1 获取 %2 个可用模型。")
                                      .arg(vendor.name).arg(names.size()));
                }
            }
            reply->deleteLater();
        });
    };

    const auto refreshVendor = [=, this](bool preserveCurrent) {
        const ModelVendor& vendor = selectedVendor();
        const QString preferred = preserveCurrent ? model->currentText() : QString();
        endpoint->setReadOnly(!vendor.custom);
        endpoint->setText(vendor.custom
            ? (vendor.id == modelVendorId_ && !modelEndpoint_.isEmpty()
                   ? modelEndpoint_ : QStringLiteral("https://your-endpoint.example/v1"))
            : vendor.endpoint);
        endpoint->setCursorPosition(0);
        apiKey->setEnabled(!vendor.local);
        apiKey->setPlaceholderText(vendor.local ? QStringLiteral("本地 Ollama 不需要 API Key")
                                                : QStringLiteral("粘贴 API Key"));
        QStringList linkParts;
        if (!vendor.accountUrl.isEmpty()) {
            linkParts << QStringLiteral("<a href=\"%1\" style=\"color:#aeb8c3\">%2</a>")
                             .arg(vendor.accountUrl,
                                  vendor.local ? QStringLiteral("安装 Ollama")
                                               : QStringLiteral("注册并申请 API Key"));
        }
        if (!vendor.tutorialUrl.isEmpty())
            linkParts << QStringLiteral("<a href=\"%1\" style=\"color:#aeb8c3\">配置教程</a>")
                             .arg(vendor.tutorialUrl);
        links->setText(linkParts.join(QStringLiteral("　·　")));
        loadFallback(vendor, QString(), preferred);
        if (vendor.custom)
            hint->setText(QStringLiteral("高级模式：填写 OpenAI 兼容 Endpoint、API Key 和模型名。"));
        else if (vendor.local)
            hint->setText(QStringLiteral("本地模式：点击获取已安装模型，资料不会离开电脑。"));
    };
    refreshVendor(false);
    if (modelVendorId_ == service->currentData().toString() &&
        !modelName_.isEmpty()) {
        if (model->findText(modelName_) < 0) model->insertItem(0, modelName_);
        model->setCurrentText(modelName_);
    }
    connect(service, &QComboBox::currentIndexChanged, &dialog, [&, refreshVendor](int) {
        apiKey->clear();
        refreshVendor(false);
        if (selectedVendor().local) QTimer::singleShot(0, &dialog, fetchModels);
    });
    connect(fetch, &QPushButton::clicked, &dialog, fetchModels);
    connect(apiKey, &QLineEdit::editingFinished, &dialog, [&, selectedVendor] {
        if (!selectedVendor().local && !apiKey->text().trimmed().isEmpty()) fetchModels();
    });
    if (selectedVendor().local) QTimer::singleShot(0, &dialog, fetchModels);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("保存"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (model->currentText().trimmed().isEmpty() || endpoint->text().trimmed().isEmpty()) {
            hint->setText(QStringLiteral("请填写模型名称和 Endpoint。"));
            return;
        }
        dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    modelVendorId_ = service->currentData().toString();
    modelService_ = service->currentText();
    modelName_ = model->currentText().trimmed();
    modelEndpoint_ = endpoint->text().trimmed();
    modelApiKey_ = apiKey->text().trimmed();
    if (modelSummary_)
        modelSummary_->setText(QStringLiteral("当前模型：%1 · %2（可在“设置 → 模型设置”中修改）")
                                   .arg(modelService_, modelName_));
}

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
    startButton_->setText(page == 3 ? QStringLiteral("生成题库安装包") : QStringLiteral("开始整理"));
    const auto steps = findChildren<QLabel*>(QStringLiteral("sideStep"));
    for (QLabel* step : steps) {
        step->setProperty("active", step->property("stepIndex").toInt() == page);
        step->style()->unpolish(step); step->style()->polish(step);
    }
}

void StudioWindow::beginPreflight() {
    if (pages_->currentIndex() == 3) {
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
