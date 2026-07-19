#include "studio_window.hpp"
#include "quizpane/diagnostic_logger.hpp"

#include "quizpane/bank_validator.hpp"
#include "quizpane/declarative_provider.hpp"
#include "quizpane/provider_installer.hpp"
#include "quizpane/running_app_handoff.hpp"
#include "quizpane/secret_store.hpp"
#include "quizpane/studio/model_client.hpp"
#include "quizpane/studio/generation_workflow.hpp"
#include "quizpane/zip_archive.hpp"
#include "source_row_widget.hpp"
#include "source_validation.hpp"
#include "styled_dropdown.hpp"

#include <QCloseEvent>
#include <QActionGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QBuffer>
#include <QColor>
#include <QCoreApplication>
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
#include <QLayout>
#include <QLineEdit>
#include <QMimeData>
#include <QMenuBar>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QJsonDocument>
#include <QJsonObject>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPdfDocument>
#include <QProgressBar>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QSet>
#include <QTimer>
#include <QSettings>
#include <QProcess>
#include <QTreeWidget>

#include <algorithm>
#include <functional>
#include <limits>
#include <utility>
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

QString studioColorTheme() {
    QSettings settings(QStringLiteral("QuizPane Project"), QStringLiteral("题库制作器"));
    const QString value = settings.value(QStringLiteral("ui/colorTheme"),
                                         QStringLiteral("dark")).toString();
    return value == QStringLiteral("light") ? value : QStringLiteral("dark");
}

void storeStudioColorTheme(const QString& value) {
    QSettings settings(QStringLiteral("QuizPane Project"), QStringLiteral("题库制作器"));
    settings.setValue(QStringLiteral("ui/colorTheme"),
                      value == QStringLiteral("light") ? value : QStringLiteral("dark"));
}

void clearLayout(QLayout* layout) {
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* widget = item->widget())
            delete widget;
        if (QLayout* nested = item->layout()) {
            clearLayout(nested);
        }
        // item 是 addLayout() 创建的 QLayoutItem，负责销毁其所持有的 nested。
        // 这里不能再 delete nested，否则切换材料清理旧标题栏时会二次释放。
        delete item;
    }
}

bool launchQuizPaneForProvider(const QString& providerEntryPath) {
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidates;
#if defined(Q_OS_MACOS)
    // 正式 macOS 包中制作器位于小窗刷题.app/Contents/Helpers；优先启动同一
    // Bundle 内的主程序，避免 open -a 因同名应用而选到 /Applications 里的旧版。
    candidates << QDir(appDir).absoluteFilePath(
        QStringLiteral("../../../../MacOS/小窗刷题"))
        // 开发构建中两个 .app 是 build/apps 下的兄弟目录。
        << QDir(appDir).absoluteFilePath(
            QStringLiteral("../../../../desktop-qt/小窗刷题.app/Contents/MacOS/小窗刷题"))
        << QStringLiteral("/Applications/小窗刷题.app/Contents/MacOS/小窗刷题");
#elif defined(Q_OS_WIN)
    candidates << QDir(appDir).filePath(QStringLiteral("小窗刷题.exe"));
#else
    candidates << QDir(appDir).filePath(QStringLiteral("小窗刷题"))
               << QStandardPaths::findExecutable(QStringLiteral("小窗刷题"));
#endif
    for (const QString& candidate : candidates) {
        if (candidate.isEmpty() || !QFileInfo(candidate).isExecutable()) continue;
        if (QProcess::startDetached(candidate, {QStringLiteral("--provider"), providerEntryPath}))
            return true;
    }
    return false;
}

QString materialPreviewHtml(const QString& text, const QJsonArray& underlines) {
    // 材料文本仍然按纯文本保存，避免把展示层 HTML 写入题库；这里只把 OCR 已识别
    // 只接受引擎根据 PDF 原始页面的文字框与水平细线交叉检测得到的字符范围；
    // 不从“【甲】”样式或子题选项猜测。没有可靠检测结果时宁可不加下划线。
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
        painter.fillRect(rect(), QColor(QStringLiteral("#0b0d10")));
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
        dragStart_ = normalized(event->position());
        selection_ = QRectF(dragStart_, QSizeF());
        update();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (!(event->buttons() & Qt::LeftButton))
            return;
        selection_ = QRectF(dragStart_, normalized(event->position())).normalized()
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

QRectF cropContextAround(const QRectF& crop) {
    const qreal horizontalPadding = qMax<qreal>(0.08, crop.width() * 0.85);
    const qreal verticalPadding = qMax<qreal>(0.10, crop.height() * 0.85);
    return QRectF(crop.x() - horizontalPadding, crop.y() - verticalPadding,
                  crop.width() + horizontalPadding * 2.0,
                  crop.height() + verticalPadding * 2.0)
        .intersected(QRectF(0.0, 0.0, 1.0, 1.0));
}

QImage cropNormalizedImage(const QImage& page, const QRectF& normalizedCrop) {
    const QRect pixels(qFloor(normalizedCrop.x() * page.width()),
                       qFloor(normalizedCrop.y() * page.height()),
                       qMax(1, qCeil(normalizedCrop.width() * page.width())),
                       qMax(1, qCeil(normalizedCrop.height() * page.height())));
    return page.copy(pixels.intersected(page.rect()));
}

QImage renderPdfReviewPage(const QString& sourcePath, int page, QString* error) {
    QPdfDocument document;
    if (document.load(sourcePath) != QPdfDocument::Error::None || page < 1 ||
        page > document.pageCount()) {
        *error = QStringLiteral("无法打开原卷第 %1 页").arg(page);
        return {};
    }
    const QSizeF points = document.pagePointSize(page - 1);
    const QSize pixels = QSize(qBound(1, qRound(points.width() * 1.7), 1800),
                               qBound(1, qRound(points.height() * 1.7), 2400));
    const QImage image = document.render(page - 1, pixels);
    if (image.isNull())
        *error = QStringLiteral("无法渲染原卷第 %1 页").arg(page);
    return image;
}

QString loadModelApiKey() {
    size_t size = 0;
    if (quizpane::SecretStore::read(QStringLiteral("question-maker"),
                                    QByteArrayLiteral("api-key"), nullptr, &size) != 0 ||
        size == 0)
        return {};
    QByteArray bytes(static_cast<qsizetype>(size), '\0');
    if (quizpane::SecretStore::read(QStringLiteral("question-maker"),
                                    QByteArrayLiteral("api-key"),
                                    reinterpret_cast<uint8_t*>(bytes.data()), &size) != 0)
        return {};
    bytes.truncate(static_cast<qsizetype>(size));
    return QString::fromUtf8(bytes);
}

ModelSettings loadStoredModelSettings() {
    QSettings settings(QStringLiteral("QuizPane Project"), QStringLiteral("题库制作器"));
    settings.beginGroup(QStringLiteral("question-maker/model"));
    ModelSettings result;
    result.vendorId = settings.value(QStringLiteral("vendorId"), result.vendorId).toString();
    result.serviceName = settings.value(QStringLiteral("serviceName"), result.serviceName).toString();
    result.modelName = settings.value(QStringLiteral("modelName"), result.modelName).toString();
    result.endpoint = settings.value(QStringLiteral("endpoint"), result.endpoint).toString();
    // 旧版没有该项。仅 OpenAI 的旧默认模型按视觉模型迁移；其他厂商默认不发送图片，
    // 让用户在模型管理中针对实际选择的模型显式确认，避免文本模型报协议错误。
    result.supportsVision = settings.contains(QStringLiteral("supportsVision"))
        ? settings.value(QStringLiteral("supportsVision")).toBool()
        : result.vendorId == QStringLiteral("openai");
    settings.endGroup();
    // 不在启动时读取钥匙串。macOS 对从 DMG 直接运行、或尚未用稳定 Developer ID
    // 签名的 App 可能每次读取都要求授权；只有用户实际调用 AI 或打开模型管理时
    // 才按需访问密钥，避免每次打开题库制作器都弹系统密码。
    return result;
}

bool storeModelSettings(const ModelSettings& value, QString* error) {
    QSettings settings(QStringLiteral("QuizPane Project"), QStringLiteral("题库制作器"));
    settings.beginGroup(QStringLiteral("question-maker/model"));
    settings.setValue(QStringLiteral("vendorId"), value.vendorId);
    settings.setValue(QStringLiteral("serviceName"), value.serviceName);
    settings.setValue(QStringLiteral("modelName"), value.modelName);
    settings.setValue(QStringLiteral("endpoint"), value.endpoint);
    settings.setValue(QStringLiteral("supportsVision"), value.supportsVision);
    settings.endGroup();
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        if (error) *error = QStringLiteral("无法保存模型的非敏感设置");
        return false;
    }
    const QByteArray key = value.apiKey.toUtf8();
    const int status = quizpane::SecretStore::write(QStringLiteral("question-maker"),
        QByteArrayLiteral("api-key"), reinterpret_cast<const uint8_t*>(key.constData()),
        static_cast<size_t>(key.size()));
    // 某些极简 Linux 环境没有 Secret Service/libsecret。仍保存非敏感配置并允许
    // 当前会话继续使用 API Key，但绝不把密钥退化写入 QSettings 明文。
    if (status == 4) {
        if (error) *error = QStringLiteral(
            "当前系统没有可用的安全凭据服务；API Key 仅在本次运行中保留，"
            "下次启动需要重新输入。");
        return true;
    }
    if (status != 0) {
        if (error) *error = QStringLiteral("无法写入系统凭据库（错误码 %1）").arg(status);
        return false;
    }
    return true;
}

}  // namespace

// ===== 应用外壳与四步向导装配 =====

StudioWindow::StudioWindow(QWidget* parent) : QMainWindow(parent) {
#if defined(Q_OS_WIN)
    // Windows 会把 QApplication 的 display name 追加到窗口标题。这里仅保留产品名，
    // 避免出现“题库制作器 · 小窗刷题 - 题库制作器”的重复标题。
    setWindowTitle(QStringLiteral("小窗刷题"));
#else
    setWindowTitle(QStringLiteral("题库制作器 · 小窗刷题"));
#endif
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
    auto* support = new QFrame;
    support->setObjectName(QStringLiteral("sidebarSupport"));
    auto* supportLayout = new QVBoxLayout(support);
    supportLayout->setContentsMargins(10, 9, 10, 9);
    supportLayout->setSpacing(2);
    auto* supportButton = new QPushButton(QStringLiteral("赞赏支持  ›"));
    supportButton->setObjectName(QStringLiteral("sidebarSupportButton"));
    supportButton->setCursor(Qt::PointingHandCursor);
    supportLayout->addWidget(supportButton);
    auto* supportHint = new QLabel(QStringLiteral("您的支持是继续更新的动力"));
    supportHint->setObjectName(QStringLiteral("sidebarSupportHint"));
    supportHint->setWordWrap(true);
    supportLayout->addWidget(supportHint);
    connect(supportButton, &QPushButton::clicked, this, &StudioWindow::showDonationDialog);
    sideLayout->addWidget(support);
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
    modelSettings_ = loadStoredModelSettings();
    updateAiReviewAffordance();
    auto* networkManager = new QNetworkAccessManager(this);
    modelClient_ = new ModelClient(networkManager, this);
    connect(modelClient_, &ModelClient::finished, this, [this](const GenerationResult& result) {
        if (aiCropInFlight_)
            handleAiCropResult(result.rawText, result.ok ? QString() : result.error);
        else
            handleAiReviewResult(result.rawText, result.ok ? QString() : result.error);
    });
    auto* settingsMenu = menuBar()->addMenu(QStringLiteral("设置"));
    settingsMenu->addAction(QStringLiteral("模型管理…"), this, &StudioWindow::editModelSettings);
    auto* appearanceMenu = settingsMenu->addMenu(QStringLiteral("外观"));
    auto* themeActions = new QActionGroup(appearanceMenu);
    themeActions->setExclusive(true);
    auto* darkThemeAction = appearanceMenu->addAction(QStringLiteral("深色模式"));
    auto* lightThemeAction = appearanceMenu->addAction(QStringLiteral("浅色模式"));
    darkThemeAction->setCheckable(true);
    lightThemeAction->setCheckable(true);
    themeActions->addAction(darkThemeAction);
    themeActions->addAction(lightThemeAction);
    const bool lightTheme = studioColorTheme() == QStringLiteral("light");
    lightThemeAction->setChecked(lightTheme);
    darkThemeAction->setChecked(!lightTheme);
    connect(darkThemeAction, &QAction::triggered, this, [this] {
        storeStudioColorTheme(QStringLiteral("dark"));
        applyStyle();
    });
    connect(lightThemeAction, &QAction::triggered, this, [this] {
        storeStudioColorTheme(QStringLiteral("light"));
        applyStyle();
    });
#ifdef QUIZPANE_DIAGNOSTIC_LOGGING
    settingsMenu->addAction(QStringLiteral("查看调试日志…"), this, [] {
        diagnostic::openLogFile();
    });
#endif
    auto* helpMenu = menuBar()->addMenu(QStringLiteral("帮助"));
    helpMenu->addAction(QStringLiteral("赞赏支持…"), this, &StudioWindow::showDonationDialog);
    applyStyle();
    updateNavigation();
}

void StudioWindow::showDonationDialog() {
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("赞赏支持"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 22, 24, 18);
    layout->setSpacing(10);
    auto* title = new QLabel(QStringLiteral("<h2 align='center'>请作者喝杯咖啡</h2>"));
    auto* description = new QLabel(QStringLiteral(
        "一个人慢慢把小窗刷题做好并不容易。您的支持，是我继续下去的最大动力。"));
    description->setAlignment(Qt::AlignCenter);
    description->setWordWrap(true);
    auto* code = new QLabel;
    code->setAlignment(Qt::AlignCenter);
    auto* caption = new QLabel;
    caption->setObjectName(QStringLiteral("muted"));
    caption->setAlignment(Qt::AlignCenter);
    auto* paymentRow = new QHBoxLayout;
    auto* previous = new QPushButton(QStringLiteral("‹"));
    auto* wechat = new QPushButton(QStringLiteral("微信支付"));
    auto* alipay = new QPushButton(QStringLiteral("支付宝"));
    auto* next = new QPushButton(QStringLiteral("›"));
    wechat->setCheckable(true);
    alipay->setCheckable(true);
    paymentRow->addStretch();
    paymentRow->addWidget(previous);
    paymentRow->addWidget(wechat);
    paymentRow->addWidget(alipay);
    paymentRow->addWidget(next);
    paymentRow->addStretch();
    bool showingAlipay = false;
    const auto updatePayment = [&] {
        const QString resource = showingAlipay
            ? QStringLiteral(":/icons/alipay-payment.jpg")
            : QStringLiteral(":/icons/wechat-payment.jpg");
        code->setPixmap(QPixmap(resource).scaled(
            220, 220, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        caption->setText(showingAlipay ? QStringLiteral("支付宝扫码赞赏")
                                       : QStringLiteral("微信扫码赞赏"));
        wechat->setChecked(!showingAlipay);
        alipay->setChecked(showingAlipay);
    };
    connect(previous, &QPushButton::clicked, &dialog, [&] {
        showingAlipay = !showingAlipay; updatePayment();
    });
    connect(next, &QPushButton::clicked, &dialog, [&] {
        showingAlipay = !showingAlipay; updatePayment();
    });
    connect(wechat, &QPushButton::clicked, &dialog, [&] {
        showingAlipay = false; updatePayment();
    });
    connect(alipay, &QPushButton::clicked, &dialog, [&] {
        showingAlipay = true; updatePayment();
    });
    updatePayment();
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    close->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(title);
    layout->addWidget(description);
    layout->addWidget(code);
    layout->addWidget(caption);
    layout->addLayout(paymentRow);
    layout->addWidget(close);
    dialog.setFixedWidth(380);
    dialog.exec();
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
    modeLayout->addWidget(new QLabel(QStringLiteral("整理方式：离线整理")));
    modeLayout->addWidget(mutedLabel(
        QStringLiteral("资料全程只在本机读取和处理，不会上传，适合题号、选项和答案比较规范的文档。")));
    layout->addWidget(modePanel);
    hasAnswerKeyCheck_ = new QCheckBox(QStringLiteral("题目资料包含答案与解析"));
    hasAnswerKeyCheck_->setChecked(true);
    hasAnswerKeyCheck_->setToolTip(QStringLiteral(
        "取消勾选会生成无答案练习题库：只保存题干、选项、材料与图片，"
        "答题后只查看和导出自己的作答结果。"));
    modeLayout->addWidget(hasAnswerKeyCheck_);
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
    sourceListContent->setObjectName(QStringLiteral("sourceListContent"));
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
        QStringLiteral("会先读取资料并检查题目结构；关闭窗口会结束本次未完成的整理。")));
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
    allReviewButton_->setCheckable(true);
    missingAnswerButton_->setCheckable(true);
    duplicateButton_->setCheckable(true);
    filters->addWidget(allReviewButton_);
    filters->addWidget(missingAnswerButton_);
    filters->addWidget(duplicateButton_);
    filters->addStretch();
    layout->addLayout(filters);
    connect(allReviewButton_, &QPushButton::clicked, this, [this] {
        activeReviewFilter_ = allReviewButton_->isChecked() ? QStringLiteral("__any_review__") : QString();
        missingAnswerButton_->setChecked(false);
        duplicateButton_->setChecked(false);
        applyReviewFilter();
    });
    connect(missingAnswerButton_, &QPushButton::clicked, this, [this] {
        activeReviewFilter_ = missingAnswerButton_->isChecked() ? QStringLiteral("__missing_answer__") : QString();
        allReviewButton_->setChecked(false);
        duplicateButton_->setChecked(false);
        applyReviewFilter();
    });
    connect(duplicateButton_, &QPushButton::clicked, this, [this] {
        activeReviewFilter_ = duplicateButton_->isChecked() ? QStringLiteral("__duplicate__") : QString();
        allReviewButton_->setChecked(false);
        missingAnswerButton_->setChecked(false);
        applyReviewFilter();
    });

    // 高危名单批量确认区：resultLevel=soft 的题目（资料分析、图形推理等规则
    // 无法验证正确性的类型）按 signals 分类展示，用户扫一眼这一类下面的样例
    // 就可以一次性放行整类，而不必逐题点开确认——批量按钮本身不是自动豁免，
    // 只是把"确认过看过这一类"的动作从 N 次点击降到 1 次。
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
        QStringLiteral("先查看左侧待确认题目；修改后点“确认本题”，再一次性生成题库。")));
    reviewTree_ = new QTreeWidget;
    reviewTree_->setColumnCount(2);
    reviewTree_->setHeaderLabels({QStringLiteral("材料 / 题目"), QStringLiteral("问题")});
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
    manualMaterialUnderlineButton_ = new QPushButton(QStringLiteral("手动标记下划线"));
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
    reviewAnswerLabel_ = new QLabel(QStringLiteral("答案选项 ID（多个答案用逗号分隔）"));
    questionEditorLayout->addWidget(reviewAnswerLabel_);
    reviewAnswerEditor_ = new QLineEdit;
    reviewAnswerEditor_->setPlaceholderText(QStringLiteral("例如：a，或 a,b"));
    questionEditorLayout->addWidget(reviewAnswerEditor_);
    reviewSolutionLabel_ = new QLabel(QStringLiteral("解析（可留空）"));
    questionEditorLayout->addWidget(reviewSolutionLabel_);
    reviewSolutionEditor_ = new QPlainTextEdit;
    reviewSolutionEditor_->setPlaceholderText(QStringLiteral("解析"));
    reviewSolutionEditor_->setMinimumHeight(80);
    questionEditorLayout->addWidget(reviewSolutionEditor_);
    auto* actions = new QHBoxLayout;
    saveReviewButton_ = new QPushButton(QStringLiteral("保存草稿"));
    saveReviewButton_->setObjectName(QStringLiteral("secondaryButton"));
    confirmReviewButton_ = new QPushButton(QStringLiteral("确认本题"));
    confirmReviewButton_->setObjectName(QStringLiteral("primaryButton"));
    excludeReviewButton_ = new QPushButton(QStringLiteral("暂不采用"));
    excludeReviewButton_->setObjectName(QStringLiteral("secondaryButton"));
    actions->addWidget(excludeReviewButton_);
    actions->addStretch();
    aiReviewButton_ = new QPushButton(QStringLiteral("AI复核"));
    aiReviewButton_->setObjectName(QStringLiteral("secondaryButton"));
    actions->addWidget(aiReviewButton_);
    actions->addWidget(saveReviewButton_);
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
    connect(reviewTree_, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* current, QTreeWidgetItem*) { showReviewQuestion(current); });
    connect(saveReviewButton_, &QPushButton::clicked, this,
            &StudioWindow::saveCurrentReviewQuestion);
    connect(confirmReviewButton_, &QPushButton::clicked, this,
            &StudioWindow::confirmCurrentReviewQuestion);
    connect(excludeReviewButton_, &QPushButton::clicked, this,
            &StudioWindow::excludeCurrentReviewQuestion);
    connect(addOptionButton, &QPushButton::clicked, this, [this] { addReviewOption(); });
    connect(aiReviewButton_, &QPushButton::clicked, this, &StudioWindow::requestAiReview);
    connect(reviewStemEditor_, &QTextEdit::textChanged,
            this, &StudioWindow::updateReviewStemHeight);
    connect(manualMaterialUnderlineButton_, &QPushButton::clicked, this,
            &StudioWindow::addManualMaterialUnderline);
    saveReviewButton_->setEnabled(false);
    confirmReviewButton_->setEnabled(false);
    excludeReviewButton_->setEnabled(false);
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

// ===== 资料列表 =====

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
            if (answerPathsByQuestion_.isEmpty()) hasAnswerKeyCheck_->setEnabled(true);
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
    hasAnswerKeyCheck_->setChecked(true);
    hasAnswerKeyCheck_->setEnabled(false);
    if (auto* row = sourceRows_.value(question)) row->setPairedAnswer(answer);
}

void StudioWindow::removeSource(const QString& question) {
    sourcePaths_.removeAll(question);
    answerPathsByQuestion_.remove(question);
    if (answerPathsByQuestion_.isEmpty()) hasAnswerKeyCheck_->setEnabled(true);
    if (auto* row = sourceRows_.take(question)) {
        sourceListLayout_->removeWidget(row);
        row->deleteLater();
    }
    sourceSummary_->setText(sourcePaths_.isEmpty() ? QStringLiteral("尚未添加文件")
        : QStringLiteral("%1 个文件").arg(sourcePaths_.size()));
    sourcePanel_->setVisible(!sourcePaths_.isEmpty());
    updateNavigation();
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
    diagnostic::event(QStringLiteral("studio"), QStringLiteral("generation-start"),
        {{QStringLiteral("mode"), QStringLiteral("rules")},
         {QStringLiteral("sources"), sourcePaths_.size()}});
    if (workflow_ && workflow_->isActive()) return;
    if (workflow_) workflow_->deleteLater();
    workflow_ = new GenerationWorkflow(this);
    connect(workflow_, &GenerationWorkflow::progressChanged,
            this, &StudioWindow::updateWorkflowProgress);
    connect(workflow_, &GenerationWorkflow::questionsReady,
            this, &StudioWindow::populateReview);
    connect(workflow_, &GenerationWorkflow::failed, this, [this](const QString& error) {
        activityTimer_->stop();
        activitySpinner_->hide();
        startButton_->setEnabled(true);
        QMessageBox::warning(this, QStringLiteral("整理未完成"), error);
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
    QList<SourceMaterialGroup> groups;
    const bool hasAnswerKey = hasAnswerKeyCheck_->isChecked();
    for (const QString& question : sourcePaths_)
        groups.append({question, answerPathsByQuestion_.value(question), hasAnswerKey});
    workflow_->startRuleBased(groups);
}

void StudioWindow::updateWorkflowProgress(const WorkflowProgress& progress) {
    int base = 0, span = 0;
    QString phase;
    switch (progress.stage) {
    case WorkflowStage::Extracting: base = 20; span = 40; phase = QStringLiteral("读取资料"); break;
    case WorkflowStage::Chunking: base = 60; span = 30; phase = QStringLiteral("规则整理"); break;
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
    generatedHasAnswerKey_ = candidate.hasAnswerKey;
    outputTokens_->setText(QString::number(generatedQuestions_.size()));
    totalTokens_->setText(QString::number(reviewQuestions_.size()));
    activeReviewFilter_.clear();
    allReviewButton_->setChecked(false);
    missingAnswerButton_->setChecked(false);
    duplicateButton_->setChecked(false);
    reviewTree_->clear();
    QHash<QString, int> softCategoryCounts;
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
        if (isSoftRisk)
            for (const QString& signal : signalList)
                ++softCategoryCounts[signal];
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

    // 人类可读的信号标签，用于批量确认区的分类标题；未在此列出的信号按原始
    // key 展示，保证新增信号不需要同步改 UI 才能显示。
    static const QHash<QString, QString> signalLabels{
        {QStringLiteral("material-type:资料分析"), QStringLiteral("资料分析")},
        {QStringLiteral("material-type:图形推理"), QStringLiteral("图形推理")},
        {QStringLiteral("image-content"), QStringLiteral("含图片内容")},
        {QStringLiteral("ocr-source"), QStringLiteral("扫描件识别")},
        {QStringLiteral("option-count-outlier"), QStringLiteral("选项数异常")},
        {QStringLiteral("answer-distribution-skew"), QStringLiteral("答案分布异常")},
        {QStringLiteral("material-layout:underline-or-blank"),
         QStringLiteral("材料含划线或填空版式")},
    };

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
            const QString reason = review.value("reason").toString();
            if (isHardRisk && reason.contains(QStringLiteral("答案"))) ++missingAnswers;
            if (isHardRisk && reason.contains(QStringLiteral("重复"))) ++duplicates;
            QStringList signalList;
            for (const auto& signal : review.value("signals").toArray())
                signalList.append(signal.toString());
            if (isSoftRisk)
                for (const QString& signal : signalList)
                    ++softCategoryCounts[signal];
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
                QStringList labels;
                for (const QString& signal : signalList)
                    labels.append(signalLabels.value(signal, signal));
                statusText = labels.join(QStringLiteral("、"));
            } else {
                statusText.clear();
            }
            const int sourceNumber = question.value("source").toObject()
                .value("questionNumber").toInt();
            const QString questionLabel = sourceNumber > 0
                ? QStringLiteral("第 %1 题").arg(sourceNumber)
                : question.value("id").toString();
            auto* item = new QTreeWidgetItem(parent, {questionLabel, statusText});
            item->setTextAlignment(0, Qt::AlignLeft | Qt::AlignVCenter);
            item->setTextAlignment(1, Qt::AlignLeft | Qt::AlignVCenter);
            item->setData(0, Qt::UserRole, question);
            item->setData(0, Qt::UserRole + 1, signalList);
            item->setData(0, Qt::UserRole + 2, isHardRisk);
            item->setData(0, Qt::UserRole + 3, isSoftRisk);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            // 所有规则已标记风险的题先留在临时草稿中，等待用户在详情面板确认；
            // 只有完全通过规则校验的题才会自动进入最终包。
            item->setCheckState(0, (isHardRisk || isSoftRisk) ? Qt::Unchecked : Qt::Checked);
            if (isSoftRisk) item->setForeground(1, QColor(QStringLiteral("#d9a441")));
        }
    };
    appendQuestions(generatedQuestions_);
    appendQuestions(reviewQuestions_);
    if (independent->childCount() == 0) delete independent;
    reviewTree_->expandToDepth(0);
    allReviewButton_->setText(QStringLiteral("全部异常  %1").arg(reviewQuestions_.size()));
    missingAnswerButton_->setText(QStringLiteral("缺少答案  %1").arg(missingAnswers));
    missingAnswerButton_->setVisible(generatedHasAnswerKey_);
    duplicateButton_->setText(QStringLiteral("疑似重复  %1").arg(duplicates));

    // 批量确认区：按 soft 信号分类展示，用户可以一次性把整类标记为已复核，
    // 不必逐题点开。类别为空（没有任何 soft 风险题）时整个面板隐藏。
    QLayoutItem* child;
    while ((child = riskCategoryLayout_->takeAt(0)) != nullptr) {
        delete child->widget();
        delete child;
    }
    if (softCategoryCounts.isEmpty()) {
        riskCategoryPanel_->setVisible(false);
    } else {
        riskCategoryLayout_->addWidget(new QLabel(QStringLiteral(
            "以下内容结构完整，但规则无法验证其视觉或语义细节。点击类别可只显示该类问题：")));
        QStringList sortedSignals = softCategoryCounts.keys();
        std::sort(sortedSignals.begin(), sortedSignals.end());
        for (const QString& signal : sortedSignals) {
            auto* row = new QHBoxLayout;
            const QString label = signalLabels.value(signal, signal);
            auto* categoryButton = new QPushButton(QStringLiteral("%1  %2 项")
                .arg(label).arg(softCategoryCounts.value(signal)));
            categoryButton->setObjectName(QStringLiteral("secondaryButton"));
            connect(categoryButton, &QPushButton::clicked, this, [this, signal] {
                activeReviewFilter_ = QStringLiteral("__signal:") + signal;
                allReviewButton_->setChecked(false);
                missingAnswerButton_->setChecked(false);
                duplicateButton_->setChecked(false);
                applyReviewFilter();
            });
            row->addWidget(categoryButton);
            row->addStretch();
            auto* confirmButton = new QPushButton(QStringLiteral("全部标记已复核"));
            confirmButton->setObjectName(QStringLiteral("reviewActionButton"));
            connect(confirmButton, &QPushButton::clicked, this,
                    [this, signal] { confirmRiskCategory(signal); });
            row->addWidget(confirmButton);
            riskCategoryLayout_->addLayout(row);
        }
        riskCategoryPanel_->setVisible(true);
    }
}

void StudioWindow::confirmRiskCategory(const QString& signal) {
    int confirmed = 0;
    std::function<void(QTreeWidgetItem*)> visit = [&](QTreeWidgetItem* item) {
        const bool isSoftRisk = item->data(0, Qt::UserRole + 3).toBool();
        const QStringList signalList = item->data(0, Qt::UserRole + 1).toStringList();
        if (isSoftRisk && signalList.contains(signal) && item->checkState(0) != Qt::Checked) {
            item->setCheckState(0, Qt::Checked);
            ++confirmed;
        }
        for (int index = 0; index < item->childCount(); ++index)
            visit(item->child(index));
    };
    for (int index = 0; index < reviewTree_->topLevelItemCount(); ++index)
        visit(reviewTree_->topLevelItem(index));
    diagnostic::event(QStringLiteral("studio"), QStringLiteral("review-category-confirmed"),
        {{QStringLiteral("signal"), signal}, {QStringLiteral("confirmed"), confirmed}});
}

void StudioWindow::displayReviewAssets(const QList<QJsonObject>& assets) {
    // titleRow 是嵌套布局。只 delete 外层 QLayoutItem 会遗留其中的按钮，切换
    // 材料后就会把“手动修正 / AI修正”一组组叠加到页面上。
    clearLayout(reviewVisualLayout_);
    QList<QJsonObject> validAssets;
    for (const QJsonObject& asset : assets) {
        const QString path = asset.value(QStringLiteral("path")).toString();
        const QByteArray bytes = generatedAssets_.value(path);
        QPixmap pixmap;
        if (path.isEmpty() || bytes.isEmpty() || !pixmap.loadFromData(bytes, "PNG"))
            continue;
        validAssets.append(asset);
    }
    if (validAssets.isEmpty()) {
        reviewVisualPanel_->setVisible(false);
        return;
    }

    // 跨页材料以前会按页重复“原卷材料版式 / 手动修正 / AI 修正”。现在把所有
    // 图片收进一个页选择器，操作仅作用于当前页，标题和按钮只保留一套。
    auto* titleRow = new QHBoxLayout;
    auto* title = mutedLabel(validAssets.first().value(QStringLiteral("alt")).toString());
    titleRow->addWidget(title, 1);
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
    auto* recrop = new QPushButton(QStringLiteral("手动修正"));
    recrop->setObjectName(QStringLiteral("reviewActionButton"));
    titleRow->addWidget(recrop);
    auto* aiCrop = new QPushButton(QStringLiteral("AI修正"));
    aiCrop->setObjectName(QStringLiteral("reviewActionButton"));
    titleRow->addWidget(aiCrop);
    reviewVisualLayout_->addLayout(titleRow);

    auto* image = new QLabel;
    image->setAlignment(Qt::AlignCenter);
    reviewVisualLayout_->addWidget(image);
    const auto selectedAsset = [validAssets, picker] {
        return validAssets.at(picker ? picker->currentIndex() : 0);
    };
    const auto showAsset = [this, validAssets, title, image, recrop, aiCrop](int index) {
        const QJsonObject asset = validAssets.at(index);
        QPixmap pixmap;
        pixmap.loadFromData(generatedAssets_.value(asset.value(QStringLiteral("path")).toString()), "PNG");
        image->setPixmap(pixmap.scaledToWidth(520, Qt::SmoothTransformation));
        image->setToolTip(asset.value(QStringLiteral("path")).toString());
        title->setText(asset.value(QStringLiteral("alt")).toString());
        const bool canRecrop = asset.value(QStringLiteral("sourcePage")).toInt() > 0 &&
            !asset.value(QStringLiteral("sourceDocument")).toString().isEmpty();
        recrop->setEnabled(canRecrop);
        // AI 修正点击时会明确说明“正在请求”或“另一项 AI 任务进行中”。不能
        // 静默禁用按钮，否则用户看起来就像点击没有反应。
        aiCrop->setEnabled(canRecrop);
        aiCrop->setText(aiCropInFlight_ ? QStringLiteral("AI修正中…")
                                         : QStringLiteral("AI修正"));
    };
    showAsset(0);
    if (picker)
        connect(picker, qOverload<int>(&QComboBox::currentIndexChanged), this, showAsset);
    connect(recrop, &QPushButton::clicked, this,
            [this, selectedAsset] { recropReviewAsset(selectedAsset()); });
    connect(aiCrop, &QPushButton::clicked, this,
            [this, selectedAsset] { requestAiCrop(selectedAsset()); });
    reviewVisualPanel_->setVisible(true);
}

void StudioWindow::recropReviewAsset(const QJsonObject& asset) {
    const QString documentName = asset.value(QStringLiteral("sourceDocument")).toString();
    const int page = asset.value(QStringLiteral("sourcePage")).toInt();
    QString sourcePath;
    for (const QString& candidate : sourcePaths_) {
        if (QFileInfo(candidate).fileName() == documentName) {
            sourcePath = candidate;
            break;
        }
    }
    if (sourcePath.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("找不到原卷"),
            QStringLiteral("找不到“%1”。请重新添加原题 PDF 后再次整理。").arg(documentName));
        return;
    }
    QString error;
    const QImage pageImage = renderPdfReviewPage(sourcePath, page, &error);
    if (pageImage.isNull()) {
        QMessageBox::warning(this, QStringLiteral("无法重新裁切"), error);
        return;
    }
    const QRectF automaticCrop = cropRectFromJson(asset.value(QStringLiteral("autoCrop")).toObject());
    const QRectF initialCrop = cropRectFromJson(asset.value(QStringLiteral("crop")).toObject());
    const QRectF fallbackCrop(0.05, 0.05, 0.90, 0.90);
    const QRectF automatic = automaticCrop.isEmpty() ? fallbackCrop : automaticCrop;
    const QRectF contextAnchor = initialCrop.isEmpty() ? automatic : initialCrop;
    CropDialog dialog(pageImage, cropContextAround(contextAnchor), contextAnchor, this);
    if (!initialCrop.isEmpty())
        dialog.setSelection(initialCrop);
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (commitReviewCrop(asset, pageImage, dialog.selection()))
        reviewDetailStatus_->setText(QStringLiteral("已从原卷第 %1 页更新截图；保存草稿后再确认本题。").arg(page));
}

bool StudioWindow::commitReviewCrop(const QJsonObject& asset, const QImage& pageImage,
                                    const QRectF& normalizedCrop) {
    const QRect crop(qFloor(normalizedCrop.x() * pageImage.width()),
                     qFloor(normalizedCrop.y() * pageImage.height()),
                     qMax(1, qCeil(normalizedCrop.width() * pageImage.width())),
                     qMax(1, qCeil(normalizedCrop.height() * pageImage.height())));
    const QImage clipped = pageImage.copy(crop.intersected(pageImage.rect()));
    QByteArray png;
    QBuffer buffer(&png);
    if (clipped.isNull() || !buffer.open(QIODevice::WriteOnly) || !clipped.save(&buffer, "PNG")) {
        QMessageBox::warning(this, QStringLiteral("无法重新裁切"), QStringLiteral("无法保存新截图。"));
        return false;
    }
    const QString path = asset.value(QStringLiteral("path")).toString();
    generatedAssets_.insert(path, png);
    QJsonObject replacement = asset;
    replacement.insert(QStringLiteral("crop"), cropRectToJson(normalizedCrop));
    QTreeWidgetItem* item = reviewTree_->currentItem();
    if (!item) return false;
    QJsonObject entry = item->data(0, Qt::UserRole).toJsonObject();
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

void StudioWindow::requestAiCrop(const QJsonObject& asset) {
    if (!ensureModelApiKeyLoaded()) {
        if (confirmAction(this, QStringLiteral("您还未配置模型"),
                          QStringLiteral("配置模型后即可使用 AI 修正。仅会上传当前裁切位置"
                                         "附近的一小块原卷区域，用于建议新的裁切框。"),
                          QStringLiteral("配置模型")))
            editModelSettings();
        return;
    }
    if (!modelClient_) {
        QMessageBox::warning(this, QStringLiteral("AI 修正不可用"),
            QStringLiteral("AI 服务尚未初始化，请关闭并重新打开题库制作器后重试。"));
        return;
    }
    if (aiCropInFlight_) {
        QMessageBox::information(this, QStringLiteral("AI 修正进行中"),
            QStringLiteral("当前题目的局部截图已经在定位中，请等待本次结果返回。"));
        return;
    }
    if (aiReviewInFlight_) {
        QMessageBox::information(this, QStringLiteral("AI 任务进行中"),
            QStringLiteral("当前有一项 AI 复核正在进行。完成后即可使用 AI 修正。"));
        return;
    }
    if (!canSendImageInput(modelSettings_)) {
        QMessageBox::information(this, QStringLiteral("当前模型不能用于 AI 修正"),
            QStringLiteral("“%1”目前未声明支持图片输入，因此没有上传任何截图。"
                           "请到“模型管理”选择视觉模型，并勾选“该模型支持图片输入（用于 AI 修正）”。")
                .arg(modelSettings_.modelName));
        return;
    }
    const QString documentName = asset.value(QStringLiteral("sourceDocument")).toString();
    const int page = asset.value(QStringLiteral("sourcePage")).toInt();
    QString sourcePath;
    for (const QString& candidate : sourcePaths_) {
        if (QFileInfo(candidate).fileName() == documentName) {
            sourcePath = candidate;
            break;
        }
    }
    if (sourcePath.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("找不到原卷"),
            QStringLiteral("找不到“%1”。请重新添加原题 PDF 后再次整理。").arg(documentName));
        return;
    }
    QString error;
    const QImage pageImage = renderPdfReviewPage(sourcePath, page, &error);
    if (pageImage.isNull()) {
        QMessageBox::warning(this, QStringLiteral("无法进行 AI 定位"), error);
        return;
    }
    const QRectF automaticCrop = cropRectFromJson(asset.value(QStringLiteral("autoCrop")).toObject());
    const QRectF savedCrop = cropRectFromJson(asset.value(QStringLiteral("crop")).toObject());
    const QRectF automatic = automaticCrop.isEmpty() ? QRectF(0.05, 0.05, 0.90, 0.90) : automaticCrop;
    const QRectF contextAnchor = savedCrop.isEmpty() ? automatic : savedCrop;
    // current crop 是原页绝对坐标锚点；模型只看它附近的局部，返回值再由 context
    // 换算回同一张原页的绝对坐标。这样无需人工先确认或标记粉色框。
    const QRectF context = cropContextAround(contextAnchor);
    const QImage contextImage = cropNormalizedImage(pageImage, context);
    QByteArray png;
    QBuffer buffer(&png);
    if (!buffer.open(QIODevice::WriteOnly) || !contextImage.save(&buffer, "PNG")) {
        QMessageBox::warning(this, QStringLiteral("无法进行 AI 定位"), QStringLiteral("无法准备局部截图。"));
        return;
    }
    const QString systemPrompt = QStringLiteral(
        "你是试卷图片裁切定位助手。找出图片中心的一整道题目的 bbox："
        "完整包含题干、图表、公式和图形选项，但不包含相邻题目。"
        "只输出 JSON："
        "{x:number,y:number,width:number,height:number,summary:string}。"
        "x/y/width/height 必须相对于这张局部图片，范围为 0 到 1。");
    const QString userContent = QStringLiteral(
        "返回图片中央这一道题的 bbox。不要解题、不要转录题目、不要生成图片；"
        "只返回裁切框 JSON。");
    pendingCropAsset_ = asset;
    pendingCropPage_ = pageImage;
    pendingCropContext_ = context;
    aiCropInFlight_ = true;
    aiReviewInFlight_ = true;
    reviewDetailStatus_->setText(QStringLiteral("正在上传当前题目附近的局部截图，请 AI 定位完整题目…"));
    updateAiReviewAffordance();
    modelClient_->generate(modelSettings_, {systemPrompt, userContent, modelSettings_.modelName, png});
}

void StudioWindow::handleAiCropResult(const QString& rawText, const QString& error) {
    aiCropInFlight_ = false;
    aiReviewInFlight_ = false;
    updateAiReviewAffordance();
    if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("AI 定位失败"), error);
        return;
    }
    QJsonParseError parseError;
    QJsonDocument document = QJsonDocument::fromJson(rawText.toUtf8(), &parseError);
    if (!document.isObject()) {
        const int first = rawText.indexOf(u'{');
        const int last = rawText.lastIndexOf(u'}');
        if (first >= 0 && last > first)
            document = QJsonDocument::fromJson(rawText.mid(first, last - first + 1).toUtf8(), &parseError);
    }
    if (!document.isObject()) {
        QMessageBox::warning(this, QStringLiteral("AI 返回格式错误"),
            QStringLiteral("未得到裁切框 JSON：%1").arg(parseError.errorString()));
        return;
    }
    const QJsonObject response = document.object();
    const QJsonObject cropObject = response.value(QStringLiteral("crop")).toObject().isEmpty()
        ? response : response.value(QStringLiteral("crop")).toObject();
    const double x = cropObject.value(QStringLiteral("x")).toDouble(-1.0);
    const double y = cropObject.value(QStringLiteral("y")).toDouble(-1.0);
    const double width = cropObject.value(QStringLiteral("width")).toDouble(-1.0);
    const double height = cropObject.value(QStringLiteral("height")).toDouble(-1.0);
    if (!cropObject.value(QStringLiteral("x")).isDouble() ||
        !cropObject.value(QStringLiteral("y")).isDouble() ||
        !cropObject.value(QStringLiteral("width")).isDouble() ||
        !cropObject.value(QStringLiteral("height")).isDouble() ||
        x < 0.0 || y < 0.0 || width < 0.01 || height < 0.01 ||
        x + width > 1.0 || y + height > 1.0) {
        QMessageBox::warning(this, QStringLiteral("AI 建议无效"),
            QStringLiteral("AI 返回的裁切范围不在局部图片内。请改用人工重新裁切。"));
        return;
    }
    const QRectF localCrop = cropRectFromJson(cropObject);
    const QRectF suggested(pendingCropContext_.x() + localCrop.x() * pendingCropContext_.width(),
                           pendingCropContext_.y() + localCrop.y() * pendingCropContext_.height(),
                           localCrop.width() * pendingCropContext_.width(),
                           localCrop.height() * pendingCropContext_.height());
    if (commitReviewCrop(pendingCropAsset_, pendingCropPage_, suggested))
        reviewDetailStatus_->setText(QStringLiteral("已应用 AI 定位结果并重新渲染原卷截图；可继续手动修正或确认本题。"));
}

void StudioWindow::setReviewOptions(const QJsonArray& options) {
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

void StudioWindow::addReviewOption(const QString& requestedId, const QString& text) {
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
    auto* remove = new QPushButton(QStringLiteral("×"));
    remove->setObjectName(QStringLiteral("secondaryButton"));
    remove->setToolTip(QStringLiteral("删除此选项"));
    remove->setFixedWidth(28);
    layout->addWidget(badge);
    layout->addWidget(editor, 1);
    layout->addWidget(remove);
    reviewOptionsLayout_->addWidget(row);
    reviewOptionEditors_.append(editor);
    connect(remove, &QPushButton::clicked, this, [this, editor, row] {
        reviewOptionEditors_.removeAll(editor);
        reviewOptionsLayout_->removeWidget(row);
        row->deleteLater();
    });
}

QJsonArray StudioWindow::reviewOptions() const {
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

void StudioWindow::showReviewQuestion(QTreeWidgetItem* item) {
    const QJsonObject entry = item ? item->data(0, Qt::UserRole).toJsonObject() : QJsonObject{};
    const bool isMaterial = entry.contains(QStringLiteral("body"));
    currentReviewItem_ = entry.isEmpty() || isMaterial ? nullptr : item;
    currentMaterialItem_ = isMaterial ? item : nullptr;
    const bool available = currentReviewItem_ != nullptr;
    saveReviewButton_->setEnabled(available);
    confirmReviewButton_->setEnabled(available);
    excludeReviewButton_->setEnabled(available);
    updateAiReviewAffordance();
    if (!available) {
        reviewStemEditor_->setReadOnly(isMaterial);
        reviewQuestionEditorPanel_->setVisible(false);
        reviewAnswerEditor_->setReadOnly(isMaterial);
        reviewSolutionEditor_->setReadOnly(isMaterial);
        if (isMaterial) {
            manualMaterialUnderlineButton_->setVisible(true);
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

    manualMaterialUnderlineButton_->setVisible(false);
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
    const int sourceNumber = question.value("source").toObject()
        .value("questionNumber").toInt();
    reviewDetailTitle_->setText(sourceNumber > 0
        ? QStringLiteral("第 %1 题").arg(sourceNumber)
        : QStringLiteral("题目 %1").arg(question.value("id").toString()));
    const QJsonObject review = question.value("review").toObject();
    QString status = item->text(1);
    if (!review.value("reason").toString().isEmpty())
        status += QStringLiteral("\n%1").arg(review.value("reason").toString());
    reviewDetailStatus_->setText(status);
    reviewStemEditor_->setPlainText(question.value("stem").toString());
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
    const QJsonObject stemImage = question.value(QStringLiteral("stemImage")).toObject();
    if (!stemImage.isEmpty())
        images.append(stemImage);
    for (const QJsonValue& value : question.value(QStringLiteral("options")).toArray()) {
        const QJsonObject image = value.toObject().value(QStringLiteral("image")).toObject();
        if (!image.isEmpty())
            images.append(image);
    }
    displayReviewAssets(images);
}

void StudioWindow::addManualMaterialUnderline() {
    if (!currentMaterialItem_)
        return;
    QJsonObject material = currentMaterialItem_->data(0, Qt::UserRole).toJsonObject();
    const QString body = material.value(QStringLiteral("body")).toString();
    if (body.isEmpty())
        return;

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("手动标记下划线"));
    dialog.setMinimumSize(620, 420);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->addWidget(mutedLabel(QStringLiteral(
        "在原始材料文本中选中需要带下划线的词句，再点击“添加所选下划线”。"
        "这只修改题库内的文字样式，不会改动原 PDF。")));
    auto* editor = new QPlainTextEdit;
    editor->setPlainText(body);
    editor->setReadOnly(true);
    editor->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    layout->addWidget(editor, 1);
    auto* buttons = new QDialogButtonBox;
    auto* add = buttons->addButton(QStringLiteral("添加所选下划线"), QDialogButtonBox::AcceptRole);
    add->setObjectName(QStringLiteral("primaryButton"));
    auto* cancel = buttons->addButton(QStringLiteral("取消"), QDialogButtonBox::RejectRole);
    connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(add, &QPushButton::clicked, &dialog, [&] {
        const QTextCursor cursor = editor->textCursor();
        const int start = cursor.selectionStart();
        const int length = cursor.selectionEnd() - start;
        if (length <= 0) {
            QMessageBox::information(&dialog, QStringLiteral("请先选择文字"),
                QStringLiteral("请在材料中选中一个词或一句话。"));
            return;
        }
        QList<QPair<int, int>> ranges;
        for (const QJsonValue& value : material.value(QStringLiteral("underlines")).toArray()) {
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
        material.insert(QStringLiteral("underlines"), merged);
        currentMaterialItem_->setData(0, Qt::UserRole, material);
        const QString id = material.value(QStringLiteral("id")).toString();
        for (int index = 0; index < generatedMaterials_.size(); ++index) {
            if (generatedMaterials_.at(index).toObject().value(QStringLiteral("id")).toString() == id) {
                generatedMaterials_[index] = material;
                break;
            }
        }
        dialog.accept();
    });
    layout->addWidget(buttons);
    if (dialog.exec() == QDialog::Accepted)
        showReviewQuestion(currentMaterialItem_);
}

bool StudioWindow::saveCurrentReviewQuestion() {
    if (!currentReviewItem_)
        return false;
    QJsonObject question = currentReviewItem_->data(0, Qt::UserRole).toJsonObject();
    const QString stem = reviewStemEditor_->toPlainText().trimmed();
    if (stem.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法保存草稿"), QStringLiteral("题干不能为空。"));
        return false;
    }

    const QJsonArray options = reviewOptions();
    if (options.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法保存草稿"), QStringLiteral("至少需要一个选项。"));
        return false;
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
            QMessageBox::warning(this, QStringLiteral("无法保存草稿"), QStringLiteral("请填写答案选项 ID。"));
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
    QJsonObject bank{{QStringLiteral("schemaVersion"), 3},
                     {QStringLiteral("title"), QStringLiteral("复核草稿")},
                     {QStringLiteral("answerPolicy"), generatedHasAnswerKey_
                         ? QStringLiteral("included") : QStringLiteral("none")},
                     {QStringLiteral("catalogs"), QJsonArray{QJsonObject{
                         {QStringLiteral("id"), QStringLiteral("generated")},
                         {QStringLiteral("title"), QStringLiteral("复核草稿")}}}},
                     {QStringLiteral("questions"), QJsonArray{question}}};
    if (!materials.isEmpty())
        bank.insert(QStringLiteral("materials"), materials);
    QString error;
    if (!quizpane::validateBank(bank, &error)) {
        QMessageBox::warning(this, QStringLiteral("草稿尚不完整"),
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

void StudioWindow::confirmCurrentReviewQuestion() {
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
}

void StudioWindow::excludeCurrentReviewQuestion() {
    if (!currentReviewItem_)
        return;
    currentReviewItem_->setCheckState(0, Qt::Unchecked);
    currentReviewItem_->setText(1, QStringLiteral("暂不采用"));
    reviewDetailStatus_->setText(QStringLiteral("本题不会进入最终题库；重新勾选或确认本题即可恢复。"));
    diagnostic::event(QStringLiteral("studio"), QStringLiteral("review-question-excluded"),
        {{QStringLiteral("questionId"), currentReviewItem_->data(0, Qt::UserRole)
            .toJsonObject().value("id").toString()}});
}

void StudioWindow::editModelSettings() {
    ModelSettings settingsForEditor = modelSettings_;
    if (settingsForEditor.vendorId != QStringLiteral("ollama"))
        settingsForEditor.apiKey = loadModelApiKey();
    const std::optional<ModelSettings> updated = quizpane::studio::editModelSettings(this, settingsForEditor);
    if (!updated)
        return;
    QString error;
    if (!storeModelSettings(*updated, &error)) {
        QMessageBox::warning(this, QStringLiteral("模型管理未保存"), error);
        return;
    }
    modelSettings_ = *updated;
    updateAiReviewAffordance();
    QMessageBox::information(this, QStringLiteral("模型管理已保存"), error.isEmpty()
        ? QStringLiteral("已保存 %1 的连接信息；API Key 已写入系统凭据库。\n"
                         "可在“设置 → 模型管理…”中打开供应商控制台查询用量，"
                         "或修改模型配置。")
              .arg(modelSettings_.serviceName)
        : error);
}

bool StudioWindow::ensureModelApiKeyLoaded() {
    if (modelSettings_.vendorId == QStringLiteral("ollama"))
        return true;
    if (!modelSettings_.apiKey.trimmed().isEmpty())
        return true;
    modelSettings_.apiKey = loadModelApiKey();
    return !modelSettings_.apiKey.trimmed().isEmpty();
}

void StudioWindow::updateAiReviewAffordance() {
    if (!aiReviewButton_)
        return;
    aiReviewButton_->setText(aiReviewInFlight_ ? QStringLiteral("AI复核中…")
                                                : QStringLiteral("AI复核"));
    aiReviewButton_->setEnabled(currentReviewItem_ && !aiReviewInFlight_);
}

void StudioWindow::requestAiReview() {
    if (!ensureModelApiKeyLoaded()) {
        if (confirmAction(this, QStringLiteral("您还未配置模型"),
                          QStringLiteral("配置模型后即可使用 AI 复核。AI 只会给出建议，"
                                         "仍需由你保存并确认。"),
                          QStringLiteral("配置模型")))
            editModelSettings();
        return;
    }
    if (!currentReviewItem_ || !modelClient_) {
        QMessageBox::information(this, QStringLiteral("选择题目"),
            QStringLiteral("请先从左侧选择一道题目，再使用 AI 复核。"));
        return;
    }
    const QJsonObject question = currentReviewItem_->data(0, Qt::UserRole).toJsonObject();
    QJsonObject context{{QStringLiteral("question"), question}};
    const QString materialId = question.value(QStringLiteral("materialId")).toString();
    for (const QJsonValue& value : generatedMaterials_) {
        const QJsonObject material = value.toObject();
        if (material.value(QStringLiteral("id")).toString() == materialId) {
            context.insert(QStringLiteral("material"), material);
            break;
        }
    }
    const bool hasVisuals = question.contains(QStringLiteral("stemImage")) || !materialId.isEmpty();
    const QString systemPrompt = QStringLiteral(
        "你是题库复核助手。只能审计给定的题目草稿，不得新增题目或虚构原文。"
        "输出 JSON：{summary:string,issues:[string],suggestedQuestion:{stem:string,"
        "options:[{id:string,text:string}],answer:{optionIds:[string]},solution:string}}。"
        "suggestedQuestion 仅在确有明确修正建议时提供；答案必须引用已有选项 ID。"
        "用户会在本地审核后决定是否应用，不能把建议视为最终答案。");
    const QString userContent = QString::fromUtf8(
        QJsonDocument(context).toJson(QJsonDocument::Compact)) +
        (hasVisuals ? QStringLiteral("\n注意：本次请求未上传原卷截图；图表、下划线和裁图正确性请只"
                                     "提示用户在本地预览核对，不可臆测图片内容。")
                    : QString());
    aiReviewInFlight_ = true;
    aiReviewButton_->setEnabled(false);
    aiReviewButton_->setText(QStringLiteral("AI复核中…"));
    modelClient_->generate(modelSettings_, {systemPrompt, userContent, modelSettings_.modelName});
}

void StudioWindow::handleAiReviewResult(const QString& rawText, const QString& error) {
    aiReviewInFlight_ = false;
    updateAiReviewAffordance();
    if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("AI 检查失败"), error);
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(rawText.toUtf8(), &parseError);
    if (!document.isObject()) {
        QMessageBox::warning(this, QStringLiteral("AI 返回格式错误"),
            QStringLiteral("未得到可读取的 JSON 建议：%1").arg(parseError.errorString()));
        return;
    }
    const QJsonObject response = document.object();
    QStringList issues;
    for (const QJsonValue& value : response.value(QStringLiteral("issues")).toArray())
        issues.append(value.toString());
    const QString summary = response.value(QStringLiteral("summary")).toString();
    const QJsonObject suggestion = response.value(QStringLiteral("suggestedQuestion")).toObject();
    QMessageBox dialog(this);
    dialog.setWindowTitle(QStringLiteral("AI 复核建议"));
    dialog.setIcon(QMessageBox::Information);
    dialog.setText(summary.isEmpty() ? QStringLiteral("AI 未发现明确的结构性问题。") : summary);
    if (!issues.isEmpty())
        dialog.setInformativeText(issues.join(QStringLiteral("\n• ")).prepend(QStringLiteral("• ")));
    dialog.setDetailedText(rawText);
    QPushButton* apply = nullptr;
    if (!suggestion.isEmpty())
        apply = dialog.addButton(QStringLiteral("填入建议，继续人工确认"), QMessageBox::AcceptRole);
    dialog.addButton(QStringLiteral("关闭"), QMessageBox::RejectRole);
    dialog.exec();
    if (dialog.clickedButton() != apply)
        return;
    reviewStemEditor_->setPlainText(suggestion.value(QStringLiteral("stem")).toString(
        reviewStemEditor_->toPlainText()));
    if (suggestion.value(QStringLiteral("options")).isArray()) {
        setReviewOptions(suggestion.value(QStringLiteral("options")).toArray());
    }
    if (suggestion.value(QStringLiteral("answer")).isObject()) {
        QStringList ids;
        for (const QJsonValue& value : suggestion.value(QStringLiteral("answer")).toObject()
                                      .value(QStringLiteral("optionIds")).toArray())
            ids.append(value.toString());
        reviewAnswerEditor_->setText(ids.join(QStringLiteral(", ")));
    }
    if (suggestion.contains(QStringLiteral("solution")))
        reviewSolutionEditor_->setPlainText(suggestion.value(QStringLiteral("solution")).toString());
    reviewDetailStatus_->setText(QStringLiteral(
        "AI 建议已填入草稿，尚未生效；请核对后保存草稿并确认本题。"));
}

void StudioWindow::updateReviewStemHeight() {
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

void StudioWindow::applyReviewFilter() {
    std::function<bool(QTreeWidgetItem*)> visit = [&](QTreeWidgetItem* item) -> bool {
        const auto matches = [this](QTreeWidgetItem* candidate) {
            if (activeReviewFilter_.isEmpty()) return true;
            const bool hardRisk = candidate->data(0, Qt::UserRole + 2).toBool();
            const bool softRisk = candidate->data(0, Qt::UserRole + 3).toBool();
            if (activeReviewFilter_ == QStringLiteral("__any_review__"))
                return hardRisk || softRisk;
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
        // 命中的是材料本身时，只显示材料行；命中子题时保留其父级路径。
        const bool visible = selfMatches || anyChildVisible;
        item->setHidden(!visible);
        if (selfMatches && !activeReviewFilter_.isEmpty() && item->childCount() > 0) {
            for (int index = 0; index < item->childCount(); ++index)
                item->child(index)->setHidden(true);
        }
        return visible;
    };
    for (int index = 0; index < reviewTree_->topLevelItemCount(); ++index)
        visit(reviewTree_->topLevelItem(index));
}

void StudioWindow::packageProvider() {
    QList<QJsonObject> selectedObjects;
    QHash<QString, int> sourceOrder;
    int sourceOrdinal = 0;
    const auto rememberSourceOrder = [&](const QJsonArray& questions) {
        for (const auto& value : questions)
            sourceOrder.insert(value.toObject().value("id").toString(), sourceOrdinal++);
    };
    rememberSourceOrder(generatedQuestions_);
    rememberSourceOrder(reviewQuestions_);
    QSet<QString> usedMaterialIds;
    for (int topIndex = 0; topIndex < reviewTree_->topLevelItemCount(); ++topIndex) {
        QTreeWidgetItem* group = reviewTree_->topLevelItem(topIndex);
        for (int childIndex = 0; childIndex < group->childCount(); ++childIndex) {
            QTreeWidgetItem* child = group->child(childIndex);
            if (child->checkState(0) == Qt::Unchecked) continue;
            const QJsonObject question = child->data(0, Qt::UserRole).toJsonObject();
            if (question.isEmpty()) continue;
            selectedObjects.append(question);
            const QString materialId = question.value("materialId").toString();
            if (!materialId.isEmpty()) usedMaterialIds.insert(materialId);
        }
    }
    if (selectedObjects.isEmpty()) {
        diagnostic::event(QStringLiteral("studio"), QStringLiteral("package-rejected"),
            {{QStringLiteral("reason"), QStringLiteral("no-selected-questions")}});
        QMessageBox::warning(this, QStringLiteral("无法生成"), QStringLiteral("至少需要采纳一道题。"));
        return;
    }
    std::stable_sort(selectedObjects.begin(), selectedObjects.end(),
        [&sourceOrder](const QJsonObject& left, const QJsonObject& right) {
            return sourceOrder.value(left.value("id").toString(), std::numeric_limits<int>::max()) <
                   sourceOrder.value(right.value("id").toString(), std::numeric_limits<int>::max());
        });
    QJsonArray selected;
    for (const QJsonObject& question : std::as_const(selectedObjects))
        selected.append(question);
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
    QJsonObject bank{{"schemaVersion", 3}, {"title", title},
        {"answerPolicy", generatedHasAnswerKey_ ? QStringLiteral("included") : QStringLiteral("none")},
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
        {"runtime", QJsonObject{{"format", "quizpane.bank+json"}, {"schemaVersion", 3},
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
    // 制作器与小窗是独立进程。先把题库交给已有的小窗进程，它会直接切换题库并
    // 进入“选择练习数量”页；绝不能通过 open -a 再拉一个同名应用实例，否则
    // macOS 可能命中另一份旧安装包，进而显示错误的 Schema 不匹配提示。
    QSettings practiceSettings(QStringLiteral("QuizPane Project"), QStringLiteral("小窗刷题"));
    practiceSettings.setValue(QStringLiteral("provider/lastLibraryPath"), installed.entryPath);
    practiceSettings.sync();
    QString handoffError;
    const bool handedOff = quizpane::handoffProviderToRunningApp(installed.entryPath, &handoffError);
    const bool launched = handedOff || launchQuizPaneForProvider(installed.entryPath);
    if (!launched) {
        QMessageBox::critical(this, QStringLiteral("无法打开小窗刷题"),
            handoffError.isEmpty()
                ? QStringLiteral("未找到可启动的小窗刷题。请确认主程序仍在安装包内。")
                : handoffError + QStringLiteral("；且未找到可启动的小窗刷题。"));
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
    QMessageBox::information(this, QStringLiteral("题库已添加"), handedOff
        ? QStringLiteral("“%1”已在当前小窗刷题中打开，请选择练习题数。制作器将关闭。").arg(title)
        : QStringLiteral("“%1”已打开到小窗刷题，请选择练习题数。制作器将关闭。").arg(title));
    QTimer::singleShot(0, this, &QWidget::close);
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
    if (!confirmAction(this, QStringLiteral("结束正在整理？"),
                       QStringLiteral("本次本地整理尚未完成。关闭后需要重新开始整理。"),
                       QStringLiteral("结束并关闭"))) {
        event->ignore();
        return;
    }
    workflow_->cancel();
    event->accept();
}

void StudioWindow::applyStyle() {
    const QString path = studioColorTheme() == QStringLiteral("light")
        ? QStringLiteral(":/styles/studio-light.qss")
        : QStringLiteral(":/styles/studio.qss");
    QFile style(path);
    if (!style.open(QIODevice::ReadOnly)) {
        qWarning("Unable to load embedded studio stylesheet");
        return;
    }
    setStyleSheet(QString::fromUtf8(style.readAll()));
}

}  // namespace quizpane::studio
