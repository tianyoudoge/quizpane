#include "main_window.hpp"
#include "../app_settings.hpp"
#if defined(QUIZPANE_HAVE_WEBSOCKETS)
#include "../browser/browser_bridge.hpp"
#endif
#include "../platform/global_hotkey.hpp"
#include "../platform/window_pinning.hpp"
#include "../platform/update_asset.hpp"
#include "app_dialogs.hpp"
#include "quizpane/diagnostic_logger.hpp"
#include "quizpane/pending_call.hpp"
#include "quizpane/provider_response_router.hpp"
#include "line_icons.hpp"
#include "material_card.hpp"
#include "question_navigator.hpp"
#include "result_image_preview.hpp"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QButtonGroup>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImage>
#include <QIcon>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QMouseEvent>
#include <QNetworkReply>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QPushButton>
#include <QPixmap>
#include <QPainter>
#include <QProcess>
#include <QProgressDialog>
#include <QRadioButton>
#include <QCheckBox>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScreen>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QSysInfo>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <exception>

#include "qrcodegen.hpp"

namespace quizpane {
namespace {

using ui::LineIcon;
using ui::makeLineIcon;

class VerticalResizeHandle final : public QWidget {
public:
    explicit VerticalResizeHandle(QWidget* parent = nullptr) : QWidget(parent) {
        setCursor(Qt::SizeVerCursor);
        setFixedHeight(4);
        setToolTip(QStringLiteral("上下拖动调整答题区高度"));
    }

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && window() &&
            window()->windowHandle()) {
            window()->windowHandle()->startSystemResize(Qt::BottomEdge);
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }
};

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

QString choiceLabel(int choice) {
    return choice >= 0 ? QString(QChar(u'A' + choice)) : QStringLiteral("未作答");
}

int countAnswered(const QVector<QSet<int>>& answers) {
    return static_cast<int>(std::count_if(answers.cbegin(), answers.cend(),
        [](const QSet<int>& choices) { return !choices.isEmpty(); }));
}

QString formatVideoTime(qint64 seconds) {
    if (seconds < 0) return QStringLiteral("--:--");
    const qint64 hours = seconds / 3600;
    const qint64 minutes = (seconds % 3600) / 60;
    const qint64 remainder = seconds % 60;
    if (hours > 0)
        return QStringLiteral("%1:%2:%3")
            .arg(hours).arg(minutes, 2, 10, QChar(u'0')).arg(remainder, 2, 10, QChar(u'0'));
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QChar(u'0')).arg(remainder, 2, 10, QChar(u'0'));
}

QString userFacingError(const QJsonObject& error) {
    const int code = error.value(QStringLiteral("data")).toObject()
        .value(QStringLiteral("networkError")).toInt(QNetworkReply::NoError);
    switch (static_cast<QNetworkReply::NetworkError>(code)) {
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::RemoteHostClosedError:
        return QStringLiteral("网络连接中断，请稍后重试");
    case QNetworkReply::HostNotFoundError:
        return QStringLiteral("无法连接题库服务，请检查网络");
    case QNetworkReply::TimeoutError:
        return QStringLiteral("网络请求超时，请检查网络后重试");
    case QNetworkReply::SslHandshakeFailedError:
        return QStringLiteral("题库服务的安全连接验证失败");
    case QNetworkReply::ProxyConnectionRefusedError:
    case QNetworkReply::ProxyConnectionClosedError:
    case QNetworkReply::ProxyNotFoundError:
    case QNetworkReply::ProxyTimeoutError:
        return QStringLiteral("代理服务器连接失败，请检查网络代理设置");
    default:
        return error.value(QStringLiteral("message")).toString(
            QStringLiteral("题库请求失败"));
    }
}

constexpr auto kReleaseMetadataUrl = "https://xutianyou.cc/quizpane/api/releases/latest";
constexpr auto kReleaseDownloadBaseUrl = "https://xutianyou.cc/quizpane/download";

int compareVersionTags(QString left, QString right) {
    left.remove(QChar(u'v'), Qt::CaseInsensitive);
    right.remove(QChar(u'v'), Qt::CaseInsensitive);
    const QStringList leftParts = left.split(QChar(u'.'));
    const QStringList rightParts = right.split(QChar(u'.'));
    const int componentCount = std::max(leftParts.size(), rightParts.size());
    for (int index = 0; index < componentCount; ++index) {
        bool leftOk = false;
        bool rightOk = false;
        const int leftPart = index < leftParts.size() ? leftParts.at(index).toInt(&leftOk) : 0;
        const int rightPart = index < rightParts.size() ? rightParts.at(index).toInt(&rightOk) : 0;
        if ((index < leftParts.size() && !leftOk) ||
            (index < rightParts.size() && !rightOk)) return 0;
        if (leftPart != rightPart) return leftPart < rightPart ? -1 : 1;
    }
    return 0;
}

QString updateAssetForCurrentPlatform() {
#if defined(QUIZPANE_WINDOWS7_COMPAT) || QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    constexpr bool qt5OrWin7 = true;
#else
    constexpr bool qt5OrWin7 = false;
#endif
    return quizpane::updateAssetForPlatform(
        quizpane::ProviderInstaller::currentPlatformKey(), qt5OrWin7);
}

QString updateWorkingDirectory(const QString& tag) {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    return QDir(base).filePath(QStringLiteral("quizpane-update-%1-%2")
        .arg(tag, QUuid::createUuid().toString(QUuid::WithoutBraces)));
}

QString updateScriptLogPath() {
    const QString directory = QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(directory);
    return QDir(directory).filePath(QStringLiteral("update.log"));
}

int scaledAlpha(int alpha, int visibility) {
    const int scaled = (alpha * qBound(0, visibility, 100) + 50) / 100;
    // Qt Widgets 在 Windows 上不能稳定地将 alpha 为 1/2 的白色子控件背景
    // 合成到半透明顶层窗口，结果会突兀地回退成白块。低于可见阈值时明确使用
    // 透明色；不要留下"几乎透明"的 rgba 值。
    return scaled < 3 ? 0 : scaled;
}

QString backgroundVisibilityStyle(bool light, int visibility) {
    const int card = scaledAlpha(light ? 242 : 224, visibility);
    const int cardBorder = scaledAlpha(light ? 42 : 0, visibility);
    const int surface = scaledAlpha(light ? 10 : 10, visibility);
    const int material = scaledAlpha(light ? 8 : 9, visibility);
    const int button = scaledAlpha(light ? 10 : 12, visibility);
    const int buttonHover = scaledAlpha(light ? 13 : 22, visibility);
    const int border = scaledAlpha(light ? 35 : 14, visibility);
    const int optionSelected = scaledAlpha(light ? 16 : 38, visibility);
    const int scroll = scaledAlpha(light ? 70 : 62, visibility);
    const QString cardRgb = light ? QStringLiteral("250,251,253")
                                  : QStringLiteral("12,14,18");
    const QString surfaceRgb = light ? QStringLiteral("47,78,108")
                                     : QStringLiteral("255,255,255");
    const QString buttonRgb = light ? QStringLiteral("47,78,108")
                                    : QStringLiteral("255,255,255");
    const QString borderRgb = light ? QStringLiteral("55,74,92")
                                    : QStringLiteral("255,255,255");
    const QString selectedRgb = light ? QStringLiteral("48,104,158")
                                      : QStringLiteral("180,188,198");
    const QString scrollRgb = light ? QStringLiteral("63,79,95")
                                    : QStringLiteral("220,225,232");

    // 只覆盖实际铺底的控件。文字颜色不参与透明度计算，因此滑到 0 时仍保持
    // 清晰可读；导航弹窗和系统菜单属于临时操作界面，维持主题默认底色。
    return QStringLiteral(R"QSS(
QWidget#card { background: rgba(%1,%2); border-color: rgba(%3,%4); }
QWidget#card QScrollBar::handle:vertical { background: rgba(%5,%6); }
QWidget#card QLabel#questionText,
QWidget#card QWidget#catalogRow,
QWidget#card QLabel#resultAnswer { background: rgba(%7,%8); }
QWidget#card QLabel#solutionText { background: rgba(%9,%10); }
QWidget#card QWidget#materialCard { background: rgba(%7,%11); }
QWidget#card QPushButton#materialImagePreview {
    background: rgba(%12,%13); border-color: rgba(%14,%15);
}
QWidget#card QPushButton { background: rgba(%16,%17); border-color: rgba(%14,%15); }
QWidget#card QPushButton:hover { background: rgba(%16,%18); }
QWidget#card QFrame#answerOptionCard { background: rgba(%7,%8); }
QWidget#card QFrame#answerOptionCard[checked="true"] { background: rgba(%19,%20); }
QWidget#card QPushButton#questionNumberButton { border-color: rgba(%14,%15); }
QWidget#card QPushButton#questionNumberButton:hover { background: rgba(%16,%18); }
)QSS")
        .arg(cardRgb).arg(card)
        .arg(light ? QStringLiteral("92,108,124") : QStringLiteral("0,0,0")).arg(cardBorder)
        .arg(scrollRgb).arg(scroll)
        .arg(surfaceRgb).arg(surface)
        .arg(light ? QStringLiteral("49,112,73") : QStringLiteral("28,36,33"))
        .arg(scaledAlpha(light ? 9 : 48, visibility))
        .arg(material)
        .arg(light ? QStringLiteral("39,56,72") : QStringLiteral("0,0,0"))
        .arg(scaledAlpha(light ? 5 : 20, visibility))
        .arg(borderRgb).arg(scaledAlpha(light ? 26 : 18, visibility))
        .arg(buttonRgb).arg(button)
        .arg(buttonHover)
        .arg(selectedRgb).arg(optionSelected);
}

}  // namespace

// ===== 窗口与页面装配 =====

MainWindow::~MainWindow() {
#if defined(QUIZPANE_HAVE_WEBSOCKETS)
    // BrowserBridge 持有 QWebSocketServer；其析构会触发客户端断开与 statusChanged。
    // 此时课程页控件可能已经被 QObject 的子对象树销毁，必须先断开 UI 槽并主动
    // 销毁 bridge，避免关闭程序时对悬空 QLabel 写入。
    if (browserBridge_ != nullptr) {
        QObject::disconnect(browserBridge_, nullptr, this, nullptr);
        browserBridge_->stop();
        delete browserBridge_;
        browserBridge_ = nullptr;
    }
#endif
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    // Qt Widgets 通过父子对象树管理控件生命周期；窗口析构时会递归释放子控件。
    pinned_ = AppSettings::windowPinned();
    backgroundVisibility_ = AppSettings::backgroundVisibility();
    Qt::WindowFlags flags = Qt::FramelessWindowHint |
                            Qt::NoDropShadowWindowHint | Qt::Tool;
    if (pinned_) flags |= Qt::WindowStaysOnTopHint;
    setWindowFlags(flags);
    setAttribute(Qt::WA_TranslucentBackground);
    setAcceptDrops(true);
    resize(420, 640);

    card_ = new QWidget(this);
    card_->setObjectName("card");
    auto* layout = new QVBoxLayout(card_);
    layout->setContentsMargins(24, 20, 24, 24);
    layout->setSpacing(14);

    headerBar_ = new QWidget;
    headerBar_->setObjectName(QStringLiteral("titleBar"));
    auto* header = new QHBoxLayout(headerBar_);
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(4);
    sourceLabel_ = new QLabel(QStringLiteral("小窗刷题"));
    sourceLabel_->setObjectName(QStringLiteral("sourceTitle"));
    menuButton_ = new QPushButton;
    menuButton_->setObjectName(QStringLiteral("headerIconButton"));
    menuButton_->setIcon(makeLineIcon(LineIcon::Menu));
    menuButton_->setAccessibleName(QStringLiteral("主菜单"));
    menuButton_->setToolTip(QStringLiteral("题库、老板键和关于"));
    menuButton_->setFixedSize(28, 28);
    sizeButton_ = new QPushButton;
    sizeButton_->setObjectName(QStringLiteral("headerIconButton"));
    sizeButton_->setIcon(makeLineIcon(LineIcon::Resize));
    sizeButton_->setAccessibleName(QStringLiteral("调整界面大小"));
    sizeButton_->setToolTip(QStringLiteral("调整界面大小"));
    sizeButton_->setFixedSize(28, 28);
    pinButton_ = new QPushButton;
    pinButton_->setObjectName(QStringLiteral("pinButton"));
    pinButton_->setIcon(makeLineIcon(LineIcon::Pin));
    pinButton_->setCheckable(true);
    pinButton_->setChecked(pinned_);
    pinButton_->setAccessibleName(QStringLiteral("始终置顶"));
    pinButton_->setFixedSize(28, 28);
    pinButton_->setToolTip(QStringLiteral("保持窗口显示在其他窗口上方"));
    auto* closeButton = new QPushButton;
    closeButton->setIcon(makeLineIcon(LineIcon::Close));
    closeButton->setAccessibleName(QStringLiteral("隐藏窗口"));
    closeButton->setFixedSize(28, 28);
    closeButton->setObjectName("closeButton");
    connect(closeButton, &QPushButton::clicked, this,
            &MainWindow::toggleWindowVisibility);
    header->addWidget(sourceLabel_);
    header->addStretch();
    header->addWidget(menuButton_);
    header->addWidget(sizeButton_);
    header->addWidget(pinButton_);
    header->addWidget(closeButton);
    // 信号带 bool 参数，槽函数签名在编译时校验。
    connect(pinButton_, &QPushButton::toggled, this, &MainWindow::setPinned);
    connect(sizeButton_, &QPushButton::clicked, this, &MainWindow::showUiSizeMenu);
    connect(menuButton_, &QPushButton::clicked, this, &MainWindow::showMainMenu);

    layout->addWidget(headerBar_);

    pages_ = new QStackedWidget;
    pages_->setObjectName(QStringLiteral("pages"));

    loginPage_ = new QWidget;
    auto* loginLayout = new QVBoxLayout(loginPage_);
    loginLayout->setContentsMargins(0, 8, 0, 0);
    loginLayout->setSpacing(8);
    titleLabel_ = new QLabel(QStringLiteral("导入题库后即可开始练习"));
    titleLabel_->setObjectName("title");
    titleLabel_->setWordWrap(true);
    detailLabel_ = new QLabel(
        QStringLiteral("拖入题库安装包，或点击下方按钮选择文件。"));
    detailLabel_->setObjectName("detail");
    detailLabel_->setWordWrap(true);
    detailLabel_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    qrLabel_ = new QLabel;
    qrLabel_->setAlignment(Qt::AlignCenter);
    qrLabel_->setVisible(false);
    actionButton_ = new QPushButton(QStringLiteral("导入题库"));
    actionButton_->setFixedHeight(32);
    createBankButton_ = new QPushButton(QStringLiteral("制作自己的题库"));
    createBankButton_->setObjectName(QStringLiteral("secondaryButton"));
    createBankButton_->setFixedHeight(32);

    loginLayout->addWidget(titleLabel_);
    loginLayout->addWidget(qrLabel_);
    loginLayout->setAlignment(qrLabel_, Qt::AlignHCenter);
    loginLayout->addWidget(detailLabel_);
    loginLayout->addWidget(actionButton_);
    loginLayout->addWidget(createBankButton_);
    connect(actionButton_, &QPushButton::clicked, this,
            &MainWindow::runPrimaryAction);
    connect(createBankButton_, &QPushButton::clicked, this, &MainWindow::openBankStudio);

    catalogPage_ = new QWidget;
    catalogController_.init(catalogPage_, pages_, provider_, loginPage_, titleLabel_, detailLabel_,
                            [this] { applyUiSize(uiSize_); });
    catalogController_.buildInto();

    // PracticePageController needs this pointer as soon as the first question is
    // shown. The handle is added to the outer layout after the pages below.
    resizeHandle_ = new VerticalResizeHandle(card_);
    resizeHandle_->setObjectName(QStringLiteral("verticalResizeHandle"));
    resizeHandle_->setVisible(false);

    practicePage_ = new QWidget;
#if defined(QUIZPANE_HAVE_WEBSOCKETS)
    auto* practiceLayoutForVideo = new QVBoxLayout(practicePage_);
    practiceLayoutForVideo->setContentsMargins(0, 0, 0, 0);
    practiceVideoStatusBar_ = new QFrame;
    practiceVideoStatusBar_->setObjectName(QStringLiteral("courseStatusBar"));
    auto* videoStatusLayout = new QHBoxLayout(practiceVideoStatusBar_);
    videoStatusLayout->setContentsMargins(9, 6, 7, 6);
    videoStatusLayout->setSpacing(7);
    practiceVideoStatusLabel_ = new QLabel;
    practiceVideoStatusLabel_->setObjectName(QStringLiteral("courseStatusText"));
    practiceVideoStatusLabel_->setWordWrap(false);
    practiceVideoDetailsButton_ = new QPushButton(QStringLiteral("详情"));
    practiceVideoDetailsButton_->setObjectName(QStringLiteral("courseStatusButton"));
    practiceVideoDetailsButton_->setFixedHeight(26);
    videoStatusLayout->addWidget(practiceVideoStatusLabel_, 1);
    videoStatusLayout->addWidget(practiceVideoDetailsButton_);
    practiceVideoStatusBar_->hide();
    connect(practiceVideoDetailsButton_, &QPushButton::clicked, this,
            &MainWindow::showCourseCompanion);
    practiceLayoutForVideo->addWidget(practiceVideoStatusBar_);
#endif
    practiceController_.init(practicePage_, pages_, catalogPage_, headerBar_, card_, resizeHandle_,
                             provider_, draftStore_, session_, uiSize_, providerId_,
                             draftRestoreChecked_);
    practiceController_.buildInto();

#if defined(QUIZPANE_HAVE_WEBSOCKETS)
    courseCompanionPage_ = new QWidget;
    auto* courseLayout = new QVBoxLayout(courseCompanionPage_);
    courseLayout->setContentsMargins(0, 8, 0, 0);
    courseLayout->setSpacing(8);
    auto* courseHeading = new QLabel(QStringLiteral("小窗刷题网课伴侣 · 已连接"));
    courseHeading->setObjectName(QStringLiteral("pageTitle"));
    courseCompanionTitleLabel_ = new QLabel(QStringLiteral("正在读取课程信息…"));
    courseCompanionTitleLabel_->setObjectName(QStringLiteral("courseTitle"));
    courseCompanionTitleLabel_->setWordWrap(true);
    courseCompanionStatusLabel_ = new QLabel;
    courseCompanionStatusLabel_->setObjectName(QStringLiteral("detail"));
    courseCompanionStatusLabel_->setWordWrap(true);
    courseCompanionProgressLabel_ = new QLabel;
    courseCompanionProgressLabel_->setObjectName(QStringLiteral("detail"));
    courseCompanionNoticeLabel_ = new QLabel;
    courseCompanionNoticeLabel_->setObjectName(QStringLiteral("detail"));
    courseCompanionNoticeLabel_->setWordWrap(true);
    courseCompanionNoticeLabel_->hide();
    auto* courseControls = new QVBoxLayout;
    courseControls->setContentsMargins(0, 0, 0, 0);
    courseControls->setSpacing(8);
    courseTogglePlaybackButton_ = new QPushButton(QStringLiteral("暂停"));
    coursePermissionButton_ = new QPushButton(QStringLiteral("授权屏幕录制"));
    coursePermissionButton_->setObjectName(QStringLiteral("secondaryButton"));
    coursePermissionButton_->hide();
    auto* continuePracticeButton = new QPushButton(QStringLiteral("继续刷题"));
    continuePracticeButton->setObjectName(QStringLiteral("secondaryButton"));
    courseControls->addWidget(courseTogglePlaybackButton_);
    courseControls->addWidget(coursePermissionButton_);
    courseLayout->addWidget(courseHeading);
    courseLayout->addWidget(courseCompanionTitleLabel_);
    courseLayout->addWidget(courseCompanionStatusLabel_);
    courseLayout->addWidget(courseCompanionProgressLabel_);
    courseLayout->addWidget(courseCompanionNoticeLabel_);
    courseLayout->addLayout(courseControls);
    courseLayout->addWidget(continuePracticeButton);
    connect(courseTogglePlaybackButton_, &QPushButton::clicked, this,
            [this] { browserBridge_->requestTogglePlayback(); });
    connect(coursePermissionButton_, &QPushButton::clicked, this, [this] {
        browserBridge_->requestScreenCapturePermission();
    });
    connect(continuePracticeButton, &QPushButton::clicked, this,
            &MainWindow::continueFromCourseCompanion);
#endif

    solutionPage_ = new QWidget;
    solutionController_.init(solutionPage_, pages_, catalogPage_, provider_, session_, uiSize_,
                             [this] { applyUiSize(uiSize_); });
    solutionController_.buildInto();

    pages_->addWidget(loginPage_);
    pages_->addWidget(catalogPage_);
    pages_->addWidget(practicePage_);
#if defined(QUIZPANE_HAVE_WEBSOCKETS)
    pages_->addWidget(courseCompanionPage_);
#endif
    pages_->addWidget(solutionPage_);
    layout->addWidget(pages_, 1);
    layout->addWidget(resizeHandle_);
    connect(pages_, &QStackedWidget::currentChanged, this, [this] {
        if (practiceController_.navigator()) practiceController_.navigator()->hide();
        resizeHandle_->setVisible(pages_->currentWidget() == practicePage_);
        QTimer::singleShot(0, this, &MainWindow::adjustWindowForCurrentPage);
    });
    loginPollTimer_ = new QTimer(this);
    loginPollTimer_->setSingleShot(true);
    connect(loginPollTimer_, &QTimer::timeout, this, &MainWindow::pollLogin);
    setCentralWidget(card_);
    applyCardStyle();
    const QString savedSize = AppSettings::uiSize();
    applyUiSize(savedSize == QStringLiteral("small") ? UiSize::Small
                : savedSize == QStringLiteral("large") ? UiSize::Large
                                                         : UiSize::Medium);
    updateNetworkManager_ = new QNetworkAccessManager(this);
    initializeDesktopShell();
    processPendingProviderDeletions();

    connect(&provider_, &ProviderLoader::responseReceived, this,
            &MainWindow::handleProviderResponse);
    connect(&provider_, &ProviderLoader::providerLog, this,
            [this](int, const QString& message) { detailLabel_->setText(message); });

    if (const auto* screen = QApplication::primaryScreen()) {
        const QRect area = screen->availableGeometry();
        move(area.right() - width() - 16, area.top() + 16);
    }
}

// ===== 桌面外壳：托盘、老板键和系统菜单 =====

void MainWindow::initializeDesktopShell() {
#if defined(QUIZPANE_HAVE_WEBSOCKETS)
    browserBridge_ = new browser::BrowserBridge(this);
    QString bridgeError;
    if (!browserBridge_->start(&bridgeError))
        qWarning("Browser bridge unavailable: %s", qPrintable(bridgeError));
#endif
    globalHotkey_ = new GlobalHotkey(this);
    connect(globalHotkey_, &GlobalHotkey::activated, this,
            &MainWindow::toggleWindowVisibility);
    const QString savedHotkey = AppSettings::bossKey();
    bossKey_ = QKeySequence::fromString(savedHotkey, QKeySequence::PortableText);
    if (bossKey_.isEmpty()) bossKey_ = QKeySequence(QStringLiteral("Ctrl+H"));
    QString hotkeyError;
    const bool globalHotkeyRegistered =
        globalHotkey_->registerBossKey(bossKey_, &hotkeyError);
    if (!globalHotkeyRegistered)
        qWarning("Boss key unavailable: %s", qPrintable(hotkeyError));

    showHideAction_ = new QAction(QStringLiteral("隐藏窗口"), this);
    showHideAction_->setShortcut(bossKey_);
    showHideAction_->setShortcutContext(Qt::ApplicationShortcut);
    addAction(showHideAction_);
    connect(showHideAction_, &QAction::triggered, this,
            &MainWindow::toggleWindowVisibility);
    pinAction_ = new QAction(QStringLiteral("始终置顶"), this);
    pinAction_->setCheckable(true);
    pinAction_->setChecked(pinned_);
    connect(pinAction_, &QAction::toggled, pinButton_, &QPushButton::setChecked);

    trayMenu_ = new QMenu(this);
    trayMenu_->addAction(showHideAction_);
    trayMenu_->addAction(pinAction_);
#if defined(QUIZPANE_HAVE_WEBSOCKETS)
    auto* browserMenu = trayMenu_->addMenu(QStringLiteral("网课视频"));
    auto* browserStatusAction = browserMenu->addAction(QStringLiteral("状态：浏览器扩展未连接"));
    browserStatusAction->setEnabled(false);
    auto* togglePlaybackAction = browserMenu->addAction(QStringLiteral("暂停 / 继续"));
    togglePlaybackAction->setEnabled(false);
    connect(togglePlaybackAction, &QAction::triggered, this,
            [this] { browserBridge_->requestTogglePlayback(); });
    connect(browserBridge_, &browser::BrowserBridge::statusChanged, this,
            [this, browserStatusAction, togglePlaybackAction](const browser::BrowserStatus& status) {
                updateCourseCompanion(status);
                if (status.connection != browser::ConnectionState::Connected) {
                    browserStatusAction->setText(QStringLiteral("状态：浏览器扩展未连接"));
                    togglePlaybackAction->setEnabled(false);
                    return;
                }
                const QString videoState = status.videoPlaying
                    ? QStringLiteral("正在播放") : QStringLiteral("已连接");
                browserStatusAction->setText(QStringLiteral("状态：%1").arg(videoState));
                togglePlaybackAction->setEnabled(status.courseBound && status.videoDetected);
            });
#endif
    trayMenu_->addAction(QStringLiteral("添加题库…"), this,
                         &MainWindow::chooseProviderPackage);
    trayMenu_->addAction(QStringLiteral("老板键设置…"), this,
                         &MainWindow::configureBossKey);
    trayMenu_->addAction(QStringLiteral("问题反馈…"), this,
                         [this] { ui::showFeedback(this); });
#ifdef QUIZPANE_DIAGNOSTIC_LOGGING
    QAction* debugLogAction = trayMenu_->addAction(QStringLiteral("查看调试日志…"));
    connect(debugLogAction, &QAction::triggered, this, [] {
        diagnostic::openLogFile();
    });
#endif
    auto* traySize = trayMenu_->addMenu(QStringLiteral("界面大小"));
    QAction* traySmall = traySize->addAction(QStringLiteral("小"));
    QAction* trayMedium = traySize->addAction(QStringLiteral("中"));
    QAction* trayLarge = traySize->addAction(QStringLiteral("大"));
    connect(traySmall, &QAction::triggered, this,
            [this] { applyUiSize(UiSize::Small); });
    connect(trayMedium, &QAction::triggered, this,
            [this] { applyUiSize(UiSize::Medium); });
    connect(trayLarge, &QAction::triggered, this,
            [this] { applyUiSize(UiSize::Large); });
    auto addAppearanceActions = [this](QMenu* menu) {
        auto* actions = new QActionGroup(menu);
        actions->setExclusive(true);
        auto* dark = menu->addAction(QStringLiteral("深色模式"));
        auto* light = menu->addAction(QStringLiteral("浅色模式"));
        dark->setCheckable(true);
        light->setCheckable(true);
        actions->addAction(dark);
        actions->addAction(light);
        const bool isLight = AppSettings::colorTheme() == QStringLiteral("light");
        light->setChecked(isLight);
        dark->setChecked(!isLight);
        connect(dark, &QAction::triggered, this, [this] {
            AppSettings::setColorTheme(QStringLiteral("dark"));
            applyCardStyle();
        });
        connect(light, &QAction::triggered, this, [this] {
            AppSettings::setColorTheme(QStringLiteral("light"));
            applyCardStyle();
        });
        menu->addSeparator();
        menu->addAction(QStringLiteral("背景透明度…"), this,
                        &MainWindow::showBackgroundVisibilityDialog);
    };
    auto* trayAppearance = trayMenu_->addMenu(QStringLiteral("外观"));
    addAppearanceActions(trayAppearance);
    trayMenu_->addAction(QStringLiteral("返回练习列表"), this,
                         &MainWindow::returnToCatalog);
    trayMenu_->addAction(QStringLiteral("赞赏支持…"), this,
                         [this] { ui::showDonation(this); });
    trayMenu_->addAction(QStringLiteral("检查更新…"), this,
                         &MainWindow::checkForUpdates);
    trayMenu_->addAction(QStringLiteral("关于小窗刷题"), this,
                         &MainWindow::showAboutDialog);
    trayMenu_->addSeparator();
    QAction* quitAction = trayMenu_->addAction(QStringLiteral("退出程序"));
    quitAction->setMenuRole(QAction::QuitRole);
    connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);

    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        trayIcon_ = new QSystemTrayIcon(QApplication::windowIcon(), this);
        trayIcon_->setToolTip(QStringLiteral("小窗刷题 · %1")
            .arg(bossKey_.toString(QKeySequence::NativeText)));
        trayIcon_->setContextMenu(trayMenu_);
        connect(trayIcon_, &QSystemTrayIcon::activated, this,
                [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger ||
                reason == QSystemTrayIcon::DoubleClick)
                toggleWindowVisibility();
        });
        trayIcon_->show();
    }

#if defined(Q_OS_MACOS)
    QMenuBar* nativeBar = menuBar();
    nativeBar->setNativeMenuBar(true);
    QMenu* appMenu = nativeBar->addMenu(QStringLiteral("小窗刷题"));
    appMenu->addAction(QStringLiteral("导入题库…"), this,
                       &MainWindow::chooseProviderPackage);
    appMenu->addAction(QStringLiteral("老板键设置…"), this,
                       &MainWindow::configureBossKey);
    appMenu->addAction(QStringLiteral("问题反馈…"), this,
                       [this] { ui::showFeedback(this); });
#ifdef QUIZPANE_DIAGNOSTIC_LOGGING
    appMenu->addAction(debugLogAction);
#endif
    appMenu->addAction(QStringLiteral("赞赏支持…"), this,
                       [this] { ui::showDonation(this); });
    appMenu->addAction(QStringLiteral("检查更新…"), this,
                       &MainWindow::checkForUpdates);
    appMenu->addAction(QStringLiteral("关于小窗刷题"), this,
                       &MainWindow::showAboutDialog);
    appMenu->addSeparator();
    appMenu->addAction(showHideAction_);
    appMenu->addSeparator();
    appMenu->addAction(quitAction);
    QMenu* windowMenu = nativeBar->addMenu(QStringLiteral("窗口"));
    windowMenu->addAction(pinAction_);
    QMenu* nativeSize = windowMenu->addMenu(QStringLiteral("界面大小"));
    QAction* nativeSmall = nativeSize->addAction(QStringLiteral("小"));
    QAction* nativeMedium = nativeSize->addAction(QStringLiteral("中"));
    QAction* nativeLarge = nativeSize->addAction(QStringLiteral("大"));
    connect(nativeSmall, &QAction::triggered, this,
            [this] { applyUiSize(UiSize::Small); });
    connect(nativeMedium, &QAction::triggered, this,
            [this] { applyUiSize(UiSize::Medium); });
    connect(nativeLarge, &QAction::triggered, this,
            [this] { applyUiSize(UiSize::Large); });
    QMenu* nativeAppearance = windowMenu->addMenu(QStringLiteral("外观"));
    addAppearanceActions(nativeAppearance);
    QMenu* practiceMenu = nativeBar->addMenu(QStringLiteral("练习"));
    practiceMenu->addAction(QStringLiteral("返回练习列表"), this,
                            &MainWindow::returnToCatalog);
#endif
}

#if defined(QUIZPANE_HAVE_WEBSOCKETS)
void MainWindow::updateCourseCompanion(const browser::BrowserStatus& status) {
    const bool justBound = status.courseBound && !browserCourseWasBound_;
    browserCourseWasBound_ = status.courseBound;

    if (status.connection != browser::ConnectionState::Connected) {
        courseCompanionTitleLabel_->setText(QStringLiteral("浏览器扩展未连接"));
        courseCompanionStatusLabel_->setText(QStringLiteral("打开网课伴侣插件并绑定课程后，这里会显示播放信息。"));
        courseCompanionProgressLabel_->setText({});
        courseCompanionNoticeLabel_->hide();
        courseTogglePlaybackButton_->setEnabled(false);
        coursePermissionButton_->hide();
        practiceVideoStatusBar_->hide();
        return;
    }

    if (!status.courseBound) {
        courseCompanionTitleLabel_->setText(QStringLiteral("尚未绑定课程页面"));
        courseCompanionStatusLabel_->setText(QStringLiteral("请在 Chrome 或 Edge 的课程播放页中打开网课伴侣插件并绑定。"));
        courseCompanionProgressLabel_->setText({});
        courseCompanionNoticeLabel_->hide();
        courseTogglePlaybackButton_->setEnabled(false);
        coursePermissionButton_->hide();
        practiceVideoStatusBar_->hide();
        return;
    }

    const QString title = status.courseTitle.isEmpty()
        ? QStringLiteral("当前课程") : status.courseTitle;
    const QString playback = !status.videoDetected ? QStringLiteral("未检测到视频")
        : status.videoPlaying ? QStringLiteral("● 正在播放") : QStringLiteral("● 已暂停");
    const bool screenCapturePermissionProblem = status.externalWindowError.contains(
        QStringLiteral("TCC"), Qt::CaseInsensitive) ||
        status.externalWindowError.contains(QStringLiteral("屏幕录制")) ||
        status.externalWindowError.contains(QStringLiteral("捕捉"));
    const QString windowNotice = screenCapturePermissionProblem
        ? QStringLiteral("视频小窗需要屏幕录制权限。请点下方按钮授权后再试。")
        : QString();
    const QString progress = QStringLiteral("播放进度 %1 / %2")
        .arg(formatVideoTime(status.videoCurrentTimeSeconds),
             formatVideoTime(status.videoDurationSeconds));

    courseCompanionTitleLabel_->setText(title);
    courseCompanionStatusLabel_->setText(QStringLiteral("%1 · %2")
        .arg(playback, status.browserName.isEmpty() ? QStringLiteral("浏览器") : status.browserName));
    courseCompanionProgressLabel_->setText(progress);
    courseCompanionNoticeLabel_->setText(windowNotice);
    courseCompanionNoticeLabel_->setVisible(!windowNotice.isEmpty());
    courseTogglePlaybackButton_->setText(status.videoPlaying ? QStringLiteral("暂停")
                                                          : QStringLiteral("继续播放"));
    courseTogglePlaybackButton_->setEnabled(status.videoDetected);
    coursePermissionButton_->setText(QStringLiteral("重新授权"));
    coursePermissionButton_->setVisible(screenCapturePermissionProblem);

    practiceVideoStatusLabel_->setText(QStringLiteral("网课伴侣 · %1 · %2 / %3")
        .arg(status.videoPlaying ? QStringLiteral("正在播放") : QStringLiteral("已暂停"),
             formatVideoTime(status.videoCurrentTimeSeconds),
             formatVideoTime(status.videoDurationSeconds)));
    practiceVideoStatusBar_->setVisible(true);

    if (justBound) showCourseCompanion();
    else if (pages_->currentWidget() == courseCompanionPage_) fitCourseCompanionWindow();
}

void MainWindow::showCourseCompanion() {
    if (!courseCompanionPage_ || !browserBridge_ || !browserBridge_->status().courseBound) return;
    if (pages_->currentWidget() != courseCompanionPage_)
        pageBeforeCourseCompanion_ = pages_->currentWidget();
    pages_->setCurrentWidget(courseCompanionPage_);
    fitCourseCompanionWindow();
}

void MainWindow::fitCourseCompanionWindow() {
    if (!courseCompanionPage_ || pages_->currentWidget() != courseCompanionPage_) return;

    // 课程页是信息面板：按当前文字和按钮的实际高度收紧，不沿用答题页的空白高度。
    auto* root = qobject_cast<QVBoxLayout*>(card_->layout());
    auto* courseLayout = qobject_cast<QVBoxLayout*>(courseCompanionPage_->layout());
    if (!root || !courseLayout) return;
    const QPoint oldTopRight = frameGeometry().topRight();
    const QSize compactSize = standardWindowSize_.isValid()
        ? standardWindowSize_ : QSize(380, 560);

    resize(compactSize.width(), height());
    courseLayout->invalidate();
    courseLayout->activate();
    const QMargins rootMargins = root->contentsMargins();
    const int totalHeight = rootMargins.top() + rootMargins.bottom() +
        headerBar_->height() + root->spacing() + courseLayout->sizeHint().height();
    // 课程页只保留播放信息与两个操作，按内容收紧，避免下方留下答题页的空白。
    resize(compactSize.width(), qBound(320, totalHeight, 560));
    if (isVisible()) move(oldTopRight.x() - width() + 1, oldTopRight.y());
}

void MainWindow::continueFromCourseCompanion() {
    QWidget* destination = pageBeforeCourseCompanion_;
    if (!destination || destination == courseCompanionPage_)
        destination = session_.questions.isEmpty() ? catalogPage_ : practicePage_;
    pages_->setCurrentWidget(destination);
}
#endif

void MainWindow::toggleWindowVisibility() {
    if (visibilityToggleDebounce_.isValid() &&
        visibilityToggleDebounce_.elapsed() < 150) return;
    visibilityToggleDebounce_.restart();
    if (isVisible()) {
#if defined(QUIZPANE_HAVE_WEBSOCKETS)
        // 不等待扩展的异步结果，桌面小窗必须立即隐藏；扩展离线时 sendCommand
        // 是无操作，现有老板键行为不会被网络状态拖慢。
        browserBridge_->requestBossHide();
#endif
        hide();
        if (showHideAction_) showHideAction_->setText(QStringLiteral("显示窗口"));
        return;
    }
    show();
    platform::applyNativeWindowPin(this, pinned_);
    if (pinned_) raise();
    activateWindow();
#if defined(QUIZPANE_HAVE_WEBSOCKETS)
    browserBridge_->requestBossRestore();
#endif
    if (showHideAction_) showHideAction_->setText(QStringLiteral("隐藏窗口"));
}

void MainWindow::returnToCatalog() {
    catalogController_.returnToCatalog();
}

// ===== 登录、目录和答题请求 =====

void MainWindow::runPrimaryAction() {
    switch (actionMode_) {
    case ActionMode::InstallProvider: chooseProviderPackage(); break;
    case ActionMode::ConnectAccount: beginLogin(); break;
    case ActionMode::CancelLogin:
        loginPollTimer_->stop();
        provider_.cancel(QStringLiteral("auth-poll"));
        loginSessionId_.clear();
        qrLabel_->clear();
        qrLabel_->setVisible(false);
        titleLabel_->setText(QStringLiteral("题库账号尚未连接"));
        detailLabel_->setText(QStringLiteral("点击下方按钮，使用对应题库 App 扫码登录。"));
        setActionMode(ActionMode::ConnectAccount, QStringLiteral("连接题库账号"));
        break;
    case ActionMode::OpenCatalog: requestCatalog(); break;
    }
}

void MainWindow::beginLogin() {
    titleLabel_->setText(QStringLiteral("正在生成登录二维码…"));
    detailLabel_->setText(QStringLiteral("二维码由当前题库服务生成。"));
    qrLabel_->setVisible(false);
    qrContent_.clear();
    adjustWindowForCurrentPage();
    QString error;
    if (!provider_.request(makePendingCall(ProviderRoute::AuthBegin).build(), &error))
        detailLabel_->setText(error);
}

void MainWindow::pollLogin() {
    if (loginSessionId_.isEmpty()) return;
    QString error;
    if (!provider_.request(
            makePendingCall(ProviderRoute::AuthPoll,
                            QJsonObject{{"loginSessionId", loginSessionId_}}).build(),
            &error)) {
        detailLabel_->setText(error);
        loginPollTimer_->start(2500);
    }
}

void MainWindow::requestCatalog() {
    catalogController_.requestCatalog();
}


void MainWindow::setQrContent(const QString& content) {
    try {
        qrContent_ = content;
        const auto code = qrcodegen::QrCode::encodeText(
            content.toUtf8().constData(), qrcodegen::QrCode::Ecc::MEDIUM);
        constexpr int border = 4;
        const int targetPixels = uiSize_ == UiSize::Small ? 180
            : uiSize_ == UiSize::Large ? 240 : 210;
        const int scale = qMax(1, targetPixels / (code.getSize() + border * 2));
        const int size = (code.getSize() + border * 2) * scale;
        QImage image(size, size, QImage::Format_RGB32);
        image.fill(Qt::white);
        const QRgb black = qRgb(0, 0, 0);
        for (int y = 0; y < code.getSize(); ++y) {
            for (int dy = 0; dy < scale; ++dy) {
                auto* row = reinterpret_cast<QRgb*>(
                    image.scanLine((y + border) * scale + dy));
                for (int x = 0; x < code.getSize(); ++x) {
                    if (code.getModule(x, y))
                        std::fill_n(row + (x + border) * scale, scale, black);
                }
            }
        }
        qrLabel_->setPixmap(QPixmap::fromImage(image));
        qrLabel_->setFixedSize(size, size);
        qrLabel_->setVisible(true);
        QTimer::singleShot(0, this, &MainWindow::adjustWindowForCurrentPage);
    } catch (const std::exception&) {
        detailLabel_->setText(QStringLiteral("二维码内容过长，无法绘制"));
    }
}

void MainWindow::setActionMode(ActionMode mode, const QString& text) {
    actionMode_ = mode;
    actionButton_->setText(text);
    if (pages_->currentWidget() == loginPage_)
        QTimer::singleShot(0, this, &MainWindow::adjustWindowForCurrentPage);
}

// ===== Provider 异步回包路由 =====

void MainWindow::handleProviderResponse(const QJsonObject& response) {
    const ProviderResponseEnvelope envelope = routeProviderResponse(response);
    diagnostic::event(QStringLiteral("ui"), QStringLiteral("provider-response-route"),
        {{QStringLiteral("id"), envelope.id},
         {QStringLiteral("error"), response.contains(QStringLiteral("error"))}});
    if (envelope.failed) { handleProviderError(envelope); return; }
    switch (envelope.route) {
    case ProviderRoute::Initialize:    handleInitializeResponse(envelope.result); break;
    case ProviderRoute::AuthBegin:     handleAuthBeginResponse(envelope.result); break;
    case ProviderRoute::AuthPoll:      handleAuthPollResponse(envelope.result); break;
    case ProviderRoute::Catalog:       handleCatalogResponse(envelope.result); break;
    case ProviderRoute::AttemptCreate: handleAttemptCreateResponse(envelope.result); break;
    case ProviderRoute::Questions:     handleQuestionsResponse(envelope.result); break;
    case ProviderRoute::FinalSave:     practiceController_.sendSubmit(); break;
    case ProviderRoute::Submit:        draftStore_.clearAttempt(providerId_, session_.attemptId); solutionController_.requestResults(); break;
    case ProviderRoute::Report:        handleReportResponse(envelope.result); break;
    case ProviderRoute::Solutions:     handleSolutionsResponse(envelope.result); break;
    case ProviderRoute::SaveAnswer:    break;
    case ProviderRoute::Unknown:       break;
    }
}

void MainWindow::handleProviderError(const ProviderResponseEnvelope& envelope) {
    const QString message = userFacingError(envelope.error);
    detailLabel_->setText(message);
    switch (envelope.route) {
    case ProviderRoute::AuthBegin:
        titleLabel_->setText(QStringLiteral("二维码生成失败"));
        setActionMode(ActionMode::ConnectAccount, QStringLiteral("重新尝试"));
        return;
    case ProviderRoute::AuthPoll:
        if (loginSessionId_.isEmpty()) return;
        loginPollTimer_->start(3000);
        return;
    case ProviderRoute::Catalog:
        pages_->setCurrentWidget(loginPage_);
        titleLabel_->setText(QStringLiteral("登录状态已失效"));
        detailLabel_->setText(QStringLiteral("请重新使用对应题库 App 扫码连接账号。"));
        setActionMode(ActionMode::ConnectAccount, QStringLiteral("重新连接"));
        return;
    case ProviderRoute::SaveAnswer:
        return;
    default:
        QMessageBox::warning(this, QStringLiteral("操作失败"), message);
        return;
    }
}

void MainWindow::handleInitializeResponse(const QJsonObject& result) {
    if (result.value("requiresLogin").toBool() && !result.value("sessionRestored").toBool()) {
        titleLabel_->setText(QStringLiteral("题库账号尚未连接"));
        detailLabel_->setText(QStringLiteral("点击下方按钮，使用对应题库 App 扫码登录。"));
        setActionMode(ActionMode::ConnectAccount, QStringLiteral("连接题库账号"));
    } else {
        if (!practiceController_.maybeRestoreDraft()) requestCatalog();
    }
}

void MainWindow::handleAuthBeginResponse(const QJsonObject& result) {
    loginSessionId_ = result.value("loginSessionId").toString();
    setQrContent(result.value("qrContent").toString());
    titleLabel_->setText(QStringLiteral("请使用对应题库 App 扫码"));
    detailLabel_->setText(QStringLiteral("扫码后请在手机上确认登录。"));
    setActionMode(ActionMode::CancelLogin, QStringLiteral("取消登录"));
    loginPollTimer_->start(result.value("pollIntervalMs").toInt(2000));
}

void MainWindow::handleAuthPollResponse(const QJsonObject& result) {
    const QString status = result.value("status").toString();
    if (status == QStringLiteral("authenticated")) {
        loginSessionId_.clear();
        qrLabel_->setVisible(false);
        titleLabel_->setText(QStringLiteral("登录成功"));
        detailLabel_->setText(QStringLiteral("正在加载练习分类…"));
        setActionMode(ActionMode::OpenCatalog, QStringLiteral("进入练习"));
        if (!practiceController_.maybeRestoreDraft()) requestCatalog();
    } else if (status == QStringLiteral("expired")) {
        loginSessionId_.clear();
        qrLabel_->setVisible(false);
        qrContent_.clear();
        titleLabel_->setText(QStringLiteral("二维码已过期"));
        detailLabel_->setText(QStringLiteral("请重新生成二维码。"));
        setActionMode(ActionMode::ConnectAccount, QStringLiteral("刷新二维码"));
    } else {
        detailLabel_->setText(status == QStringLiteral("waiting_confirmation")
            ? QStringLiteral("已扫码，请在手机上确认登录。")
            : QStringLiteral("等待手机扫码…"));
        loginPollTimer_->start(2000);
    }
}

void MainWindow::handleCatalogResponse(const QJsonObject& result) {
    catalogController_.populateCatalog(result.value("nodes").toArray(),
        [this](const QString& catId, const QString& title, int count, bool inc) {
            practiceController_.startAttempt(catId, title, count, inc);
        });
    setActionMode(ActionMode::OpenCatalog, QStringLiteral("刷新分类"));
}

void MainWindow::handleAttemptCreateResponse(const QJsonObject& result) {
    session_.attemptId = result.value("attemptId").toString();
    session_.attemptTitle = result.value("title").toString(session_.attemptTitle);
    session_.attemptHasAnswerKey = result.value("hasAnswerKey").toBool(true);
    if (session_.attemptId.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("创建失败"), QStringLiteral("题库没有返回练习编号"));
        pages_->setCurrentWidget(catalogPage_);
        return;
    }
    practiceController_.requestQuestions();
}

void MainWindow::handleQuestionsResponse(const QJsonObject& result) {
    session_.questions = result.value("questions").toArray();
    practiceController_.updateMaterialsCache(result.value("materials").toArray());
    if (session_.questions.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("读取失败"), QStringLiteral("这套练习没有可显示的题目"));
        pages_->setCurrentWidget(catalogPage_);
        return;
    }
    practiceController_.resetForNewQuestions();
}

void MainWindow::handleReportResponse(const QJsonObject& result) {
    session_.attemptHasAnswerKey = result.value("hasAnswerKey").toBool(session_.attemptHasAnswerKey);
    if (session_.attemptHasAnswerKey) {
        solutionController_.setResultSummary(QStringLiteral("%1 / %2 题正确")
            .arg(result.value("correctCount").toInt())
            .arg(result.value("questionCount").toInt()));
    } else {
        solutionController_.setResultSummary(QStringLiteral("已作答 %1 / %2 题 · 未提供答案，不评分")
            .arg(result.value("answerCount").toInt())
            .arg(result.value("questionCount").toInt()));
        session_.solutions = session_.questions;
        if (!session_.solutions.isEmpty()) {
            solutionController_.showSolution(0);
            pages_->setCurrentWidget(solutionPage_);
        }
    }
}

void MainWindow::handleSolutionsResponse(const QJsonObject& result) {
    session_.solutions = result.value("solutions").toArray();
    practiceController_.updateMaterialsCache(result.value("materials").toArray());
    if (session_.solutions.isEmpty()) {
        solutionController_.setNoSolutionsMessage();
        return;
    }
    solutionController_.showSolution(0);
    pages_->setCurrentWidget(solutionPage_);
}

// ===== 无边框窗口事件与题库包拖放 =====

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    const auto urls = event->mimeData()->urls();
    if (urls.size() == 1 && urls.first().isLocalFile() &&
        urls.first().toLocalFile().endsWith(QStringLiteral(".quizpane-provider"),
                                            Qt::CaseInsensitive))
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event) {
    const auto urls = event->mimeData()->urls();
    if (urls.size() != 1 || !urls.first().isLocalFile()) return;
    installProviderPackage(urls.first().toLocalFile());
    event->acceptProposedAction();
}

void MainWindow::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        dragOffset_ = event->globalPosition().toPoint() - frameGeometry().topLeft();
#else
        dragOffset_ = event->globalPos() - frameGeometry().topLeft();
#endif
    }
    QMainWindow::mousePressEvent(event);
}

void MainWindow::mouseMoveEvent(QMouseEvent* event) {
    if (event->buttons().testFlag(Qt::LeftButton) && !dragOffset_.isNull()) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        move(event->globalPosition().toPoint() - dragOffset_);
#else
        move(event->globalPos() - dragOffset_);
#endif
        event->accept();
        return;
    }
    QMainWindow::mouseMoveEvent(event);
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    practiceController_.handleResize();
}

// ===== 题库安装、加载和切换 =====

void MainWindow::chooseProviderPackage() {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("导入题库"), {},
        QStringLiteral("题库安装包 (*.quizpane-provider)"));
    if (!path.isEmpty()) installProviderPackage(path);
}

void MainWindow::installProviderPackage(const QString& path) {
    diagnostic::event(QStringLiteral("package"), QStringLiteral("install-start"),
        {{QStringLiteral("file"), QFileInfo(path).fileName()}});
    ProviderPackageInfo package;
    QString error;
    if (!installer_.inspect(path, &package, &error)) {
        diagnostic::event(QStringLiteral("package"), QStringLiteral("inspect-failed"),
            {{QStringLiteral("error"), error}});
        QMessageBox::warning(this, QStringLiteral("无法导入题库"), error);
        return;
    }
    const QString permission = package.requestsNetwork
        ? QStringLiteral("需要访问网络") : QStringLiteral("不访问网络");
    if (!ui::confirm(this, QStringLiteral("确认导入题库"),
        QStringLiteral("%1\n版本：%2\n权限：%3\n\n"
                       "导入后即可在小窗刷题中使用。是否继续？")
            .arg(package.name, package.version, permission),
        QStringLiteral("导入"), QStringLiteral("取消"))) return;

    ProviderInstallResult result;
    if (!installer_.install(package, &result, &error)) {
        diagnostic::event(QStringLiteral("package"), QStringLiteral("install-failed"),
            {{QStringLiteral("id"), package.id}, {QStringLiteral("error"), error}});
        QMessageBox::warning(this, QStringLiteral("导入失败"), error);
        return;
    }
    diagnostic::event(QStringLiteral("package"), QStringLiteral("install-success"),
        {{QStringLiteral("id"), package.id}, {QStringLiteral("version"), package.version}});
    loadProvider(result.entryPath);
}

bool MainWindow::loadProvider(const QString& path) {
    diagnostic::event(QStringLiteral("ui"), QStringLiteral("load-provider"),
        {{QStringLiteral("file"), QFileInfo(path).fileName()}});
    // 切换题库前清理只属于当前页面会话的状态；安全登录态由 ProviderLoader 按
    // providerId 存在系统凭据库中，因此 unload 不等于退出登录。
    loginPollTimer_->stop();
    loginSessionId_.clear();
    qrContent_.clear();
    qrLabel_->clear();
    qrLabel_->setVisible(false);
    session_.attemptId.clear();
    session_.questions = {};
    session_.solutions = {};
    session_.attemptHasAnswerKey = true;
    session_.materialsById.clear();
    session_.answers.clear();
    if (practiceController_.navigator()) practiceController_.navigator()->hide();
    QString error;
    if (!provider_.load(path, &error)) {
        diagnostic::event(QStringLiteral("ui"), QStringLiteral("load-provider-failed"),
            {{QStringLiteral("error"), error}});
        titleLabel_->setText(QStringLiteral("题库加载失败"));
        detailLabel_->setText(error);
        return false;
    }
    const auto descriptor = provider_.descriptor();
    providerId_ = descriptor.value("id").toString();
    diagnostic::event(QStringLiteral("ui"), QStringLiteral("load-provider-success"),
        {{QStringLiteral("id"), providerId_},
         {QStringLiteral("version"), descriptor.value("version").toString()}});
    currentProviderPath_ = QFileInfo(path).absoluteFilePath();
    AppSettings::setLastProviderPath(QFileInfo(path).absoluteFilePath());
    draftRestoreChecked_ = false;
    sourceLabel_->setText(descriptor.value("name").toString(QStringLiteral("题库")));
    titleLabel_->setText(QStringLiteral("正在打开题库…"));
    detailLabel_->setText(QStringLiteral("请稍候"));
    sendInitialize();
    return true;
}

bool MainWindow::loadLastProvider() {
    // 旧版把裸动态库路径写进设置。升级后该文件可能还在，却已经无法与新版 ABI
    // 或依赖组合加载；"没有题库"绝不能因此呈现为加载失败。
    const auto installed = installer_.listInstalled();
    const QString savedPath = AppSettings::lastProviderPath();
    QString path;
    for (const InstalledProviderInfo& candidate : installed) {
        if (candidate.entryPath == savedPath) {
            path = candidate.entryPath;
            break;
        }
    }
    if (path.isEmpty() && !installed.isEmpty()) path = installed.first().entryPath;
    if (path.isEmpty()) {
        AppSettings::clearLastProviderPath();
        return false;
    }
    loadProvider(path);
    if (!provider_.isLoaded()) {
        AppSettings::clearLastProviderPath();
        showProviderOnboarding();
        return false;
    }
    return true;
}

void MainWindow::sendInitialize() {
    QString error;
    provider_.request(
        makePendingCall(ProviderRoute::Initialize,
                        QJsonObject{{"hostVersion", "0.1.0"},
                                    {"providerAbi", 1},
                                    {"locale", "zh-CN"}}).build(),
        &error);
    if (!error.isEmpty()) detailLabel_->setText(error);
}

// ===== 用户菜单与内置题库制作器 =====

void MainWindow::showUiSizeMenu() {
    QMenu menu(this);
    QAction* smallAction = menu.addAction(QStringLiteral("小"));
    QAction* mediumAction = menu.addAction(QStringLiteral("中"));
    QAction* largeAction = menu.addAction(QStringLiteral("大"));
    smallAction->setCheckable(true);
    mediumAction->setCheckable(true);
    largeAction->setCheckable(true);
    smallAction->setChecked(uiSize_ == UiSize::Small);
    mediumAction->setChecked(uiSize_ == UiSize::Medium);
    largeAction->setChecked(uiSize_ == UiSize::Large);
    QAction* selected = menu.exec(
        sizeButton_->mapToGlobal(QPoint(0, sizeButton_->height())));
    if (selected == smallAction) applyUiSize(UiSize::Small);
    else if (selected == mediumAction) applyUiSize(UiSize::Medium);
    else if (selected == largeAction) applyUiSize(UiSize::Large);
}

void MainWindow::showBackgroundVisibilityDialog() {
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("背景透明度"));
    dialog.setModal(true);

    auto* layout = new QVBoxLayout(&dialog);
    auto* description = new QLabel(
        QStringLiteral("滑到最左侧时，卡片和选项蒙版都会消失，文字保持清晰。"), &dialog);
    description->setWordWrap(true);
    layout->addWidget(description);

    auto* valueLabel = new QLabel(&dialog);
    auto* slider = new QSlider(Qt::Horizontal, &dialog);
    slider->setRange(0, 100);
    slider->setValue(backgroundVisibility_);
    slider->setTickInterval(10);
    slider->setTickPosition(QSlider::TicksBelow);
    slider->setAccessibleName(QStringLiteral("背景可见度"));
    layout->addWidget(slider);

    auto updateLabel = [valueLabel](int value) {
        valueLabel->setText(value == 0 ? QStringLiteral("纯文字")
                          : QStringLiteral("背景可见度：%1% ").arg(value));
    };
    updateLabel(slider->value());
    layout->addWidget(valueLabel);

    auto* buttons = new QHBoxLayout;
    auto* reset = new QPushButton(QStringLiteral("恢复默认"), &dialog);
    auto* done = new QPushButton(QStringLiteral("完成"), &dialog);
    buttons->addWidget(reset);
    buttons->addStretch();
    buttons->addWidget(done);
    layout->addLayout(buttons);

    int pendingValue = slider->value();
    QTimer previewTimer(&dialog);
    previewTimer.setSingleShot(true);
    connect(slider, &QSlider::valueChanged, &dialog, [this, &pendingValue, &previewTimer,
                                                        updateLabel](int value) {
        pendingValue = value;
        updateLabel(value);
        // 拖动时至多 150ms 刷一次样式；不是每个滑块像素都 repolish。
        if (!previewTimer.isActive()) previewTimer.start(150);
    });
    connect(&previewTimer, &QTimer::timeout, &dialog, [this, &pendingValue] {
        applyBackgroundVisibility(pendingValue);
    });
    connect(slider, &QSlider::sliderReleased, &dialog, [this, slider, &previewTimer] {
        previewTimer.stop();
        applyBackgroundVisibility(slider->value());
    });
    connect(reset, &QPushButton::clicked, slider, [slider] { slider->setValue(100); });
    connect(done, &QPushButton::clicked, &dialog, &QDialog::accept);

    dialog.exec();
    // 键盘调节或直接关闭对话框也必须提交最后一个值。
    applyBackgroundVisibility(slider->value());
}

void MainWindow::applyBackgroundVisibility(int value) {
    backgroundVisibility_ = qBound(0, value, 100);
    AppSettings::setBackgroundVisibility(backgroundVisibility_);
    applyCardStyle();
}

void MainWindow::showMainMenu() {
    // 菜单每次打开都重新扫描安装目录，避免缓存与磁盘状态不一致。QAction 捕获的
    // InstalledProviderInfo 是值拷贝，菜单关闭后也不会悬空。
    QMenu menu(this);
    QMenu* providerMenu = menu.addMenu(QStringLiteral("题库管理"));
    providerMenu->addAction(QStringLiteral("添加题库…"), this,
                            &MainWindow::chooseProviderPackage);
    providerMenu->addAction(QStringLiteral("制作自己的题库…"), this,
                            &MainWindow::openBankStudio);

    QString listError;
    const QList<InstalledProviderInfo> installed =
        installer_.listInstalled(&listError);
    QMenu* switchMenu = providerMenu->addMenu(QStringLiteral("切换题库"));
    QMenu* deleteMenu = providerMenu->addMenu(QStringLiteral("删除题库"));
    QMenu* exportMenu = providerMenu->addMenu(QStringLiteral("导出我的题库"));
    if (installed.isEmpty()) {
        QAction* emptySwitch = switchMenu->addAction(QStringLiteral("暂无已添加题库"));
        QAction* emptyDelete = deleteMenu->addAction(QStringLiteral("暂无可删除题库"));
        QAction* emptyExport = exportMenu->addAction(QStringLiteral("暂无可导出的本地题库"));
        emptySwitch->setEnabled(false);
        emptyDelete->setEnabled(false);
        emptyExport->setEnabled(false);
    } else {
        bool hasExportableProvider = false;
        for (const InstalledProviderInfo& item : installed) {
            QAction* switchAction = switchMenu->addAction(
                QStringLiteral("%1 · %2").arg(item.name, item.version));
            switchAction->setCheckable(true);
            switchAction->setChecked(item.id == providerId_);
            switchAction->setEnabled(item.id != providerId_);
            connect(switchAction, &QAction::triggered, this,
                    [this, item] { switchProvider(item); });
            QAction* deleteAction = deleteMenu->addAction(item.name);
            connect(deleteAction, &QAction::triggered, this,
                    [this, item] { deleteProvider(item); });
            if (item.kind == QStringLiteral("declarative")) {
                hasExportableProvider = true;
                QAction* exportAction = exportMenu->addAction(item.name);
                connect(exportAction, &QAction::triggered, this,
                        [this, item] { exportDeclarativeProvider(item); });
            }
        }
        if (!hasExportableProvider) {
            QAction* emptyExport = exportMenu->addAction(
                QStringLiteral("暂无可导出的本地题库"));
            emptyExport->setEnabled(false);
        }
    }

    menu.addSeparator();
    menu.addAction(QStringLiteral("背景透明度…"), this,
                   &MainWindow::showBackgroundVisibilityDialog);
    menu.addAction(QStringLiteral("老板键设置…"), this,
                   &MainWindow::configureBossKey);
    menu.addAction(QStringLiteral("问题反馈…"), this,
                   [this] { ui::showFeedback(this); });
    {
        auto* diagnosticsAction = menu.addAction(QStringLiteral("记录诊断日志"));
        diagnosticsAction->setCheckable(true);
        diagnosticsAction->setChecked(diagnostic::isDiagnosticsEnabled());
        connect(diagnosticsAction, &QAction::toggled, this,
                [](bool enabled) { diagnostic::setDiagnosticsEnabled(enabled); });
    }
#ifdef QUIZPANE_DIAGNOSTIC_LOGGING
    menu.addAction(QStringLiteral("查看调试日志…"), this, [] {
        diagnostic::openLogFile();
    });
#endif
    menu.addSeparator();
    menu.addAction(QStringLiteral("赞赏支持…"), this,
                   [this] { ui::showDonation(this); });
    menu.addAction(QStringLiteral("检查更新…"), this,
                   &MainWindow::checkForUpdates);
    menu.addAction(QStringLiteral("关于小窗刷题"), this,
                   &MainWindow::showAboutDialog);
    menu.exec(menuButton_->mapToGlobal(QPoint(0, menuButton_->height())));
}

void MainWindow::openBankStudio() {
    QStringList candidates;
    // 新变量与产品名一致；旧变量继续兼容已有开发环境和自动化脚本。
    QString configured = qEnvironmentVariable("QUIZPANE_QUESTION_MAKER");
    if (configured.isEmpty())
        configured = qEnvironmentVariable("QUIZPANE_BANK_STUDIO");
    if (!configured.isEmpty()) candidates.append(configured);
    const QString appDir = QCoreApplication::applicationDirPath();
#if defined(Q_OS_MACOS)
    candidates << QDir(appDir).absoluteFilePath(
        QStringLiteral("../../../../bank-studio/题库制作器.app/Contents/MacOS/题库制作器"))
        << QDir(appDir).absoluteFilePath(
            QStringLiteral("../../../题库制作器.app/Contents/MacOS/题库制作器"))
        << QDir(appDir).absoluteFilePath(
            QStringLiteral("../Helpers/题库制作器.app/Contents/MacOS/题库制作器"))
        << QStringLiteral("/Applications/题库制作器.app/Contents/MacOS/题库制作器");
#elif defined(Q_OS_WIN)
    // Win7 绿色包使用 ASCII 文件名，避开系统 ZIP 解压器的 Unicode 文件名乱码；
    // 中文名回退继续兼容常规 Windows 包和旧安装。
#if defined(QUIZPANE_WINDOWS7_COMPAT)
    candidates << QDir(appDir).filePath(QStringLiteral("QuizPaneStudio.exe"))
               << QDir(appDir).filePath(QStringLiteral("题库制作器.exe"));
#else
    candidates << QDir(appDir).filePath(QStringLiteral("题库制作器.exe"))
               << QDir(appDir).filePath(QStringLiteral("QuizPaneStudio.exe"));
#endif
#else
    candidates << QDir(appDir).filePath(QStringLiteral("题库制作器"))
               << QStandardPaths::findExecutable(QStringLiteral("题库制作器"));
#endif
    for (const QString& candidate : candidates) {
        if (candidate.isEmpty() || !QFileInfo(candidate).isExecutable()) continue;
        if (QProcess::startDetached(candidate, {})) return;
    }
    QMessageBox::warning(this, QStringLiteral("题库制作器不可用"),
        QStringLiteral("当前安装不完整，请重新安装小窗刷题。"));
}

void MainWindow::configureBossKey() {
    const auto requestedValue = ui::askBossKey(this, bossKey_);
    if (!requestedValue) return;
    const QKeySequence requested = *requestedValue;
    const QKeySequence previous = bossKey_;
    QString error;
    if (!globalHotkey_->registerBossKey(requested, &error)) {
        globalHotkey_->registerBossKey(previous);
        bossKey_ = requested;
        showHideAction_->setShortcut(bossKey_);
        AppSettings::setBossKey(bossKey_.toString(QKeySequence::PortableText));
        QMessageBox::information(this, QStringLiteral("已启用前台老板键"),
            QStringLiteral("%1\n\n当前桌面环境无法注册系统级热键；该组合键在小窗位于前台时仍可隐藏窗口。")
                .arg(error));
        return;
    }
    bossKey_ = requested;
    AppSettings::setBossKey(bossKey_.toString(QKeySequence::PortableText));
    showHideAction_->setShortcut(bossKey_);
    if (trayIcon_)
        trayIcon_->setToolTip(QStringLiteral("小窗刷题 · %1")
            .arg(bossKey_.toString(QKeySequence::NativeText)));
}

void MainWindow::showAboutDialog() {
    ui::showAbout(this);
}

void MainWindow::checkForUpdates() {
    if (updateReply_) {
        QMessageBox::information(this, QStringLiteral("检查更新"),
            QStringLiteral("正在检查或下载更新，请稍候。"));
        return;
    }
    if (updateAssetForCurrentPlatform().isEmpty()) {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://xutianyou.cc/quizpane/#download")));
        QMessageBox::information(this, QStringLiteral("检查更新"),
            QStringLiteral("当前系统请在官网下载对应的更新包。"));
        return;
    }

    QUrl metadataUrl(QString::fromLatin1(kReleaseMetadataUrl));
    QUrlQuery metadataQuery;
    metadataQuery.addQueryItem(QStringLiteral("refresh"), QStringLiteral("1"));
    metadataUrl.setQuery(metadataQuery);
    QNetworkRequest request(metadataUrl);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setRawHeader("Cache-Control", "no-cache");
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("QuizPane/%1 update-check")
                          .arg(QApplication::applicationVersion()));
    updateReply_ = updateNetworkManager_->get(request);
    QNetworkReply* reply = updateReply_;
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        if (updateReply_ != reply) return;
        updateReply_ = nullptr;
        const QByteArray body = reply->readAll();
        const QString error = reply->errorString();
        const bool requestOk = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        if (!requestOk) {
            QMessageBox message(QMessageBox::Warning, QStringLiteral("无法检查更新"),
                QStringLiteral("暂时无法连接更新服务：%1").arg(error),
                QMessageBox::NoButton, this);
            QPushButton* openDownload = message.addButton(QStringLiteral("打开下载页"),
                                                           QMessageBox::AcceptRole);
            message.addButton(QStringLiteral("稍后再试"), QMessageBox::RejectRole);
            message.exec();
            if (message.clickedButton() == openDownload)
                QDesktopServices::openUrl(QUrl(QStringLiteral("https://xutianyou.cc/quizpane/#download")));
            return;
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
        const QJsonObject release = document.isObject() ? document.object() : QJsonObject{};
        const QString tag = release.value(QStringLiteral("tag")).toString();
        const QString asset = updateAssetForCurrentPlatform();
        const QJsonObject assetInfo = release.value(QStringLiteral("assets")).toObject()
            .value(asset).toObject();
        if (parseError.error != QJsonParseError::NoError || tag.isEmpty() || assetInfo.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("无法检查更新"),
                QStringLiteral("更新服务返回的数据不完整，请前往官网下载。"));
            return;
        }
        if (compareVersionTags(QApplication::applicationVersion(), tag) >= 0) {
            QMessageBox::information(this, QStringLiteral("检查更新"),
                QStringLiteral("当前已是最新版本（%1）。")
                    .arg(QApplication::applicationVersion()));
            return;
        }
        const QString digest = assetInfo.value(QStringLiteral("sha256")).toString().toLower();
        if (digest.size() != 64) {
            QMessageBox::warning(this, QStringLiteral("更新包暂不可用"),
                QStringLiteral("新版没有可验证的 SHA-256 校验值，请前往官网下载。"));
            return;
        }
        QMessageBox message(QMessageBox::Information, QStringLiteral("发现新版本"),
            QStringLiteral("发现 %1（当前 %2）。\n\n将下载 %3，校验完成后退出并更新程序。"
                           "题库、练习记录和模型配置不会受到影响。")
                .arg(tag, QApplication::applicationVersion(), asset),
            QMessageBox::NoButton, this);
        QPushButton* update = message.addButton(QStringLiteral("立即更新"),
                                                QMessageBox::AcceptRole);
        message.addButton(QStringLiteral("稍后再说"), QMessageBox::RejectRole);
        message.exec();
        if (message.clickedButton() == update)
            downloadAndInstallUpdate(tag, asset, digest);
    });
}

void MainWindow::downloadAndInstallUpdate(const QString& tag, const QString& asset,
                                          const QString& expectedSha256) {
    const QString workDirectory = updateWorkingDirectory(tag);
    if (!QDir().mkpath(workDirectory)) {
        QMessageBox::warning(this, QStringLiteral("无法下载更新"),
            QStringLiteral("无法创建更新临时目录。"));
        return;
    }
    updateTag_ = tag;
    updateAsset_ = asset;
    updateExpectedSha256_ = expectedSha256;
    updateDownloadPath_ = QDir(workDirectory).filePath(asset);
    updateDownloadFile_ = new QFile(updateDownloadPath_, this);
    if (!updateDownloadFile_->open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, QStringLiteral("无法下载更新"),
            QStringLiteral("无法写入更新文件：%1").arg(updateDownloadFile_->errorString()));
        resetUpdateDownload();
        return;
    }

    const QUrl url(QStringLiteral("%1/%2/%3")
        .arg(QString::fromLatin1(kReleaseDownloadBaseUrl), tag,
             QString::fromUtf8(QUrl::toPercentEncoding(asset))));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("QuizPane/%1 updater")
                          .arg(QApplication::applicationVersion()));
    updateProgress_ = new QProgressDialog(QStringLiteral("正在下载更新…"),
        QStringLiteral("取消"), 0, 0, this);
    updateProgress_->setWindowTitle(QStringLiteral("小窗刷题更新"));
    updateProgress_->setWindowModality(Qt::WindowModal);
    updateProgress_->setAutoClose(false);
    updateProgress_->setAutoReset(false);
    updateProgress_->show();
    updateReply_ = updateNetworkManager_->get(request);
    QNetworkReply* reply = updateReply_;
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        if (reply != updateReply_ || !updateDownloadFile_) return;
        if (updateDownloadFile_->write(reply->readAll()) < 0) reply->abort();
    });
    connect(reply, &QNetworkReply::downloadProgress, this,
            [this](qint64 received, qint64 total) {
        if (!updateProgress_) return;
        if (total > 0) {
            updateProgress_->setRange(0, 1000);
            updateProgress_->setValue(static_cast<int>(received * 1000 / total));
            updateProgress_->setLabelText(QStringLiteral("正在下载更新… %1 / %2")
                .arg(QLocale().formattedDataSize(received), QLocale().formattedDataSize(total)));
        }
    });
    connect(updateProgress_, &QProgressDialog::canceled, reply, &QNetworkReply::abort);
    connect(reply, &QNetworkReply::finished, this, &MainWindow::completeUpdateDownload);
}

void MainWindow::completeUpdateDownload() {
    QNetworkReply* reply = updateReply_;
    updateReply_ = nullptr;
    if (!reply) return;
    if (updateDownloadFile_) {
        updateDownloadFile_->write(reply->readAll());
        updateDownloadFile_->close();
    }
    const bool downloaded = reply->error() == QNetworkReply::NoError && updateDownloadFile_;
    const QString error = reply->errorString();
    reply->deleteLater();
    if (!downloaded) {
        QMessageBox::warning(this, QStringLiteral("更新下载失败"),
            QStringLiteral("更新包未能下载完成：%1").arg(error));
        resetUpdateDownload();
        return;
    }

    QFile archive(updateDownloadPath_);
    if (!archive.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("更新校验失败"),
            QStringLiteral("无法读取下载的更新包。"));
        resetUpdateDownload();
        return;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!archive.atEnd()) hash.addData(archive.read(1024 * 1024));
    const QString actualSha256 = QString::fromLatin1(hash.result().toHex());
    if (actualSha256 != updateExpectedSha256_) {
        QMessageBox::critical(this, QStringLiteral("更新校验失败"),
            QStringLiteral("下载文件的 SHA-256 与发布记录不一致，已取消更新。"));
        resetUpdateDownload();
        return;
    }
    if (!startDownloadedUpdate()) {
        resetUpdateDownload();
        return;
    }
    QMessageBox::information(this, QStringLiteral("正在更新"),
        QStringLiteral("更新包已校验。程序退出后将自动替换并重新启动。"));
    resetUpdateDownload();
    QTimer::singleShot(0, qApp, &QApplication::quit);
}

bool MainWindow::startDownloadedUpdate() {
    const QString workDirectory = QFileInfo(updateDownloadPath_).absolutePath();
#if defined(Q_OS_WIN)
    // Win7 may only have PowerShell 2.0. Check the actual commands before
    // quitting; starting powershell.exe alone does not mean it can install ZIPs.
    const QString powershell = QStandardPaths::findExecutable(QStringLiteral("powershell.exe"));
    QProcess probe;
    if (!powershell.isEmpty()) {
        probe.start(powershell, {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
            QStringLiteral("-Command"), QStringLiteral(
                "$a = Get-Command Expand-Archive -ErrorAction SilentlyContinue; "
                "$w = Get-Command Wait-Process -ErrorAction SilentlyContinue; "
                "if ($a -and $w -and $w.Parameters.ContainsKey('Timeout')) { exit 0 }; exit 1")});
    }
    const bool canInstall = !powershell.isEmpty() && probe.waitForFinished(5000) &&
        probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0;
    if (!canInstall) {
        if (probe.state() != QProcess::NotRunning) {
            probe.kill();
            probe.waitForFinished(1000);
        }
        QMessageBox message(QMessageBox::Information, QStringLiteral("请手动完成更新"),
            QStringLiteral("当前系统缺少自动解压更新所需的组件。更新包已下载并校验，"
                           "请退出程序后，将包中的 QuizPane 文件夹解压到新目录使用。\n\n%1")
                .arg(updateDownloadPath_), QMessageBox::NoButton, this);
        auto* openFolder = message.addButton(QStringLiteral("打开更新包所在目录"), QMessageBox::AcceptRole);
        message.addButton(QStringLiteral("稍后处理"), QMessageBox::RejectRole);
        message.exec();
        if (message.clickedButton() == openFolder)
            QDesktopServices::openUrl(QUrl::fromLocalFile(workDirectory));
        return false;
    }
    const QString scriptPath = QDir(workDirectory).filePath(
        QStringLiteral("apply-update.ps1"));
    const QString destination = QCoreApplication::applicationDirPath();
    const QString restartPath = QCoreApplication::applicationFilePath();
    const QString script = QStringLiteral(R"PS(
param([string]$Package, [string]$Destination, [string]$Restart, [string]$Log, [int]$ProcessId)
$ErrorActionPreference = 'Stop'
try {
  # 等待当前进程真正退出，而非猜测 2 秒足够。否则 Windows 仍可能锁住 exe/dll，
  # 覆盖失败后只留下一个看起来"什么也没发生"的更新。
  $running = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
  if ($null -ne $running) { $running | Wait-Process -Timeout 30 -ErrorAction Stop }
  $work = Join-Path (Split-Path -Parent $Package) 'expanded'
  Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
  Expand-Archive -LiteralPath $Package -DestinationPath $work -Force
  $source = Join-Path $work 'QuizPane'
  $mainExecutable = if (Test-Path -LiteralPath (Join-Path $source 'QuizPane.exe')) {
    'QuizPane.exe'
  } elseif (Test-Path -LiteralPath (Join-Path $source '小窗刷题.exe')) {
    '小窗刷题.exe'
  } else {
    throw '更新包结构不正确'
  }
  $restartTarget = Join-Path $Destination $mainExecutable
  Get-ChildItem -LiteralPath $source -Force | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $Destination -Recurse -Force
  }
  Start-Process -FilePath $restartTarget
} catch {
  "[$(Get-Date -Format o)] $_" | Out-File -LiteralPath $Log -Append -Encoding utf8
  # 覆盖失败时恢复旧程序，避免用户更新后只看到程序消失。
  if (Test-Path -LiteralPath $Restart) { Start-Process -FilePath $Restart }
}
)PS");
    QFile scriptFile(scriptPath);
    if (!scriptFile.open(QIODevice::WriteOnly | QIODevice::Text) ||
        scriptFile.write(QByteArray::fromHex("efbbbf") + script.toUtf8()) < 0) {
        QMessageBox::warning(this, QStringLiteral("无法安装更新"),
            QStringLiteral("无法创建更新脚本。"));
        return false;
    }
    scriptFile.close();
    if (powershell.isEmpty() || !QProcess::startDetached(powershell,
            {QStringLiteral("-NoProfile"), QStringLiteral("-WindowStyle"),
             QStringLiteral("Hidden"), QStringLiteral("-ExecutionPolicy"),
             QStringLiteral("Bypass"), QStringLiteral("-File"), scriptPath,
             updateDownloadPath_, destination, restartPath, updateScriptLogPath(),
             QString::number(QCoreApplication::applicationPid())})) {
        QMessageBox::warning(this, QStringLiteral("无法安装更新"),
            QStringLiteral("无法启动 Windows 更新程序。"));
        return false;
    }
    return true;
#elif defined(Q_OS_MACOS)
    const QString scriptPath = QDir(workDirectory).filePath(
        QStringLiteral("apply-update.zsh"));
    const QString destination = QDir::cleanPath(
        QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("../../..")));
    const QFileInfo targetInfo(destination);
    if (!destination.endsWith(QStringLiteral(".app")) ||
        !QFileInfo(targetInfo.absolutePath()).isWritable()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(updateDownloadPath_));
        QMessageBox::information(this, QStringLiteral("更新包已下载"),
            QStringLiteral("当前应用目录没有写入权限，已打开 DMG。请将其中的小窗刷题拖到“应用程序”覆盖旧版本。"));
        return false;
    }
    const QString script = QStringLiteral(R"ZSH(#!/bin/zsh
set -eu
package="$1"
target="$2"
log="$3"
mount="$(mktemp -d /tmp/quizpane-update.XXXXXX)"
cleanup() { hdiutil detach "$mount" -quiet 2>/dev/null || true; rmdir "$mount" 2>/dev/null || true; }
trap cleanup EXIT
{
  sleep 2
  hdiutil attach "$package" -nobrowse -readonly -mountpoint "$mount"
  source="$mount/小窗刷题.app"
  [[ -d "$source" ]] || { echo '更新包结构不正确'; exit 1; }
  staging="${target}.updating"
  backup="${target}.previous"
  rm -rf "$staging" "$backup"
  ditto "$source" "$staging"
  [[ ! -d "$target" ]] || mv "$target" "$backup"
  if ! mv "$staging" "$target"; then
    [[ ! -d "$backup" ]] || mv "$backup" "$target"
    exit 1
  fi
  rm -rf "$backup"
  open "$target"
} >> "$log" 2>&1
)ZSH");
    QFile scriptFile(scriptPath);
    if (!scriptFile.open(QIODevice::WriteOnly | QIODevice::Text) ||
        scriptFile.write(script.toUtf8()) < 0) {
        QMessageBox::warning(this, QStringLiteral("无法安装更新"),
            QStringLiteral("无法创建更新脚本。"));
        return false;
    }
    scriptFile.close();
    scriptFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                              QFileDevice::ExeOwner);
    if (!QProcess::startDetached(QStringLiteral("/bin/zsh"),
                                 {scriptPath, updateDownloadPath_, destination,
                                  updateScriptLogPath()})) {
        QMessageBox::warning(this, QStringLiteral("无法安装更新"),
            QStringLiteral("无法启动 macOS 更新程序。"));
        return false;
    }
    return true;
#else
    Q_UNUSED(workDirectory)
    return false;
#endif
}

void MainWindow::resetUpdateDownload() {
    if (updateProgress_) {
        updateProgress_->close();
        updateProgress_->deleteLater();
        updateProgress_ = nullptr;
    }
    if (updateDownloadFile_) {
        updateDownloadFile_->deleteLater();
        updateDownloadFile_ = nullptr;
    }
    updateTag_.clear();
    updateAsset_.clear();
    updateExpectedSha256_.clear();
    updateDownloadPath_.clear();
}

void MainWindow::switchProvider(const InstalledProviderInfo& provider) {
    if (provider.id == providerId_) return;
    loadProvider(provider.entryPath);
}

void MainWindow::deleteProvider(const InstalledProviderInfo& provider) {
    const bool current = provider.id == providerId_;
    const QString currentPath = current ? currentProviderPath_ : QString{};
    const QString message = current
        ? QStringLiteral("正在使用“%1”。删除后将返回题库导入页。\n\n"
                         "登录状态会保留，重新添加后仍可恢复。确定删除吗？")
              .arg(provider.name)
        : QStringLiteral("确定删除“%1”吗？\n\n"
                         "登录状态会保留，重新添加后仍可恢复。")
              .arg(provider.name);
    if (!ui::confirm(this, QStringLiteral("删除题库"), message,
                     QStringLiteral("删除"), QStringLiteral("取消"))) return;

    if (current) {
        provider_.unload();
        providerId_.clear();
        currentProviderPath_.clear();
        AppSettings::clearLastProviderPath();
    }
    QString error;
    if (!installer_.removeInstalled(provider.id, &error)) {
        QMessageBox retryBox(QMessageBox::Warning, QStringLiteral("题库文件仍被占用"),
            QStringLiteral("暂时无法删除“%1”。\n\n原因：%2\n\n"
                           "可以立即重试，或安排在下次启动小窗刷题时清理。")
                .arg(provider.name, error), QMessageBox::NoButton, this);
        auto* retry = retryBox.addButton(QStringLiteral("立即重试清理"), QMessageBox::AcceptRole);
        auto* later = retryBox.addButton(QStringLiteral("下次启动清理"), QMessageBox::ActionRole);
        retryBox.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
        retryBox.exec();
        if (retryBox.clickedButton() == retry && installer_.removeInstalled(provider.id, &error)) {
            if (current) showProviderOnboarding();
            return;
        }
        if (retryBox.clickedButton() != later && retryBox.clickedButton() != retry) {
            if (current && !currentPath.isEmpty()) loadProvider(currentPath);
            return;
        }
        QStringList pending = AppSettings::pendingProviderDeletions();
        if (!pending.contains(provider.id)) pending.append(provider.id);
        AppSettings::setPendingProviderDeletions(pending);
        QMessageBox::information(this, QStringLiteral("已安排启动时清理"),
            QStringLiteral("下次启动“小窗刷题”时会再次删除“%1”。若仍被其他程序占用，"
                           "它会继续保留在待清理列表中。\n\n最后一次错误：%2")
                .arg(provider.name, error));
    }
    if (current) showProviderOnboarding();
}

void MainWindow::exportDeclarativeProvider(const InstalledProviderInfo& provider) {
    const QString defaultName = provider.name + QStringLiteral(".quizpane-provider");
    QString output = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出题库"), defaultName,
        QStringLiteral("QuizPane 题库 (*.quizpane-provider)"));
    if (output.isEmpty()) return;
    if (!output.endsWith(QStringLiteral(".quizpane-provider"), Qt::CaseInsensitive))
        output += QStringLiteral(".quizpane-provider");
    QString error;
    if (!installer_.exportDeclarative(provider, output, &error)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), error);
        return;
    }
    QMessageBox::information(this, QStringLiteral("已导出题库"),
                             QStringLiteral("已保存到：%1")
                                 .arg(QDir::toNativeSeparators(output)));
}

void MainWindow::showProviderOnboarding() {
    loginPollTimer_->stop();
    loginSessionId_.clear();
    qrContent_.clear();
    qrLabel_->clear();
    qrLabel_->setVisible(false);
    sourceLabel_->setText(QStringLiteral("小窗刷题"));
    titleLabel_->setText(QStringLiteral("导入题库后即可开始练习"));
    detailLabel_->setText(QStringLiteral("拖入题库安装包，或点击下方按钮选择文件。"));
    setActionMode(ActionMode::InstallProvider, QStringLiteral("导入题库"));
    pages_->setCurrentWidget(loginPage_);
    adjustWindowForCurrentPage();
}

void MainWindow::processPendingProviderDeletions() {
    const QStringList pending = AppSettings::pendingProviderDeletions();
    QStringList remaining;
    for (const QString& id : pending) {
        QString error;
        if (!installer_.removeInstalled(id, &error)) remaining.append(id);
    }
    AppSettings::setPendingProviderDeletions(remaining);
}

// ===== 窗口尺寸、置顶和视觉样式 =====

void MainWindow::applyUiSize(UiSize size) {
    // Qt 的布局系统类似浏览器 layout：先设置控件约束，再由 layout 计算最终几何。
    // 答题页保留用户手动调整的高度，登录页则按内容 sizeHint 自动收紧。
    uiSize_ = size;
    QString property;
    QSize windowSize;
    constexpr int margin = layout_metrics::kWindowMargin;
    constexpr int spacing = layout_metrics::kControlSpacing;
    constexpr int iconPixels = layout_metrics::kIconPixels;
    constexpr int iconButtonPixels = layout_metrics::kIconButtonPixels;
    constexpr int navIconPixels = layout_metrics::kNavIconPixels;
    constexpr int navIconButtonPixels = layout_metrics::kNavIconButtonPixels;
    if (size == UiSize::Small) {
        property = QStringLiteral("small");
        windowSize = QSize(320, 460);
    } else if (size == UiSize::Large) {
        property = QStringLiteral("large");
        windowSize = QSize(460, 700);
    } else {
        property = QStringLiteral("medium");
        windowSize = QSize(380, 560);
    }
    standardWindowSize_ = windowSize;

    const QPoint oldTopRight = frameGeometry().topRight();
    card_->setProperty("uiSize", property);
    card_->style()->unpolish(card_);
    card_->style()->polish(card_);
    if (auto* layout = qobject_cast<QVBoxLayout*>(card_->layout())) {
        layout->setContentsMargins(margin, 10, margin, margin);
        layout->setSpacing(spacing);
    }
    headerBar_->setFixedHeight(layout_metrics::kHeaderHeight);
    if (practiceController_.controlBar()) practiceController_.controlBar()->setFixedHeight(layout_metrics::kControlBarHeight);
    if (solutionController_.controlBar()) solutionController_.controlBar()->setFixedHeight(layout_metrics::kControlBarHeight);
    for (auto* button : card_->findChildren<QPushButton*>()) {
        const QString name = button->objectName();
        if (name != QStringLiteral("navIconButton") &&
            name != QStringLiteral("headerIconButton") &&
            name != QStringLiteral("pinButton") &&
            name != QStringLiteral("closeButton")) continue;
        const bool bottomNavigation = name == QStringLiteral("navIconButton");
        const int pixels = bottomNavigation ? navIconPixels : iconPixels;
        const int buttonPixels = bottomNavigation ? navIconButtonPixels : iconButtonPixels;
        button->setIconSize(QSize(pixels, pixels));
        button->setFixedSize(buttonPixels, buttonPixels);
    }
    if (qrLabel_->isVisible() && !qrContent_.isEmpty())
        setQrContent(qrContent_);
    if (pages_->currentWidget() == practicePage_ && !session_.questions.isEmpty()) {
        resize(windowSize.width(), height());
        if (isVisible()) move(oldTopRight.x() - width() + 1, oldTopRight.y());
        QTimer::singleShot(0, this, [this] { practiceController_.lockCompactPracticeHeight(); });
    } else if (pages_->currentWidget() == loginPage_) {
        adjustWindowForCurrentPage();
    } else {
        resize(windowSize);
        if (isVisible()) move(oldTopRight.x() - width() + 1, oldTopRight.y());
    }
    sizeButton_->setAccessibleName(QStringLiteral("界面大小：%1")
        .arg(size == UiSize::Small ? QStringLiteral("小")
             : size == UiSize::Large ? QStringLiteral("大") : QStringLiteral("中")));
    AppSettings::setUiSize(property);
}

void MainWindow::adjustWindowForCurrentPage() {
    if (!card_ || !pages_) return;
    if (pages_->currentWidget() != loginPage_) return;

    auto* root = qobject_cast<QVBoxLayout*>(card_->layout());
    auto* pageLayout = qobject_cast<QVBoxLayout*>(loginPage_->layout());
    if (!root || !pageLayout) return;
    pageLayout->invalidate();
    pageLayout->activate();

    const QMargins margins = root->contentsMargins();
    const int contentHeight = pageLayout->sizeHint().height();
    const int totalHeight = margins.top() + margins.bottom() +
                            headerBar_->height() + root->spacing() +
                            contentHeight;
    const int compactHeight = qBound(130, totalHeight,
                                     standardWindowSize_.height());
    const QPoint oldTopRight = frameGeometry().topRight();
    resize(standardWindowSize_.width(), compactHeight);
    if (isVisible()) move(oldTopRight.x() - width() + 1, oldTopRight.y());
}

void MainWindow::setPinned(bool pinned) {
    const QPoint position = pos();
    pinned_ = pinned;
    setWindowFlag(Qt::WindowStaysOnTopHint, pinned_);
    move(position);
    show();
    platform::applyNativeWindowPin(this, pinned_);
    if (pinned_) raise();
    AppSettings::setWindowPinned(pinned_);
    if (pinAction_) {
        const QSignalBlocker blocker(pinAction_);
        pinAction_->setChecked(pinned_);
    }
    pinButton_->setToolTip(pinned_
        ? QStringLiteral("已置顶，点击取消始终悬浮")
        : QStringLiteral("点击后保持在其他窗口上方"));
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    QTimer::singleShot(0, this, [this] {
        platform::applyNativeWindowPin(this, pinned_);
        if (pinned_) raise();
    });
}

void MainWindow::keyPressEvent(QKeyEvent* event) {
    // 小窗场景下双手不离键盘做几题更顺手：数字键直接选选项，方向键翻题，
    // Enter/Return 交卷。只对答题页和解析页生效，避免影响对话框默认行为。
    if (event->isAutoRepeat()) { QMainWindow::keyPressEvent(event); return; }
    QWidget* page = pages_ ? pages_->currentWidget() : nullptr;
    if (page == practicePage_) {
        const int key = event->key();
        if (key == Qt::Key_Left || key == Qt::Key_Up) {
            practiceController_.showQuestion(practiceController_.currentQuestionIndex() - 1); return;
        }
        if (key == Qt::Key_Right || key == Qt::Key_Down) {
            practiceController_.showQuestion(practiceController_.currentQuestionIndex() + 1); return;
        }
        // 数字键 1-9 / 小键盘 1-9：直接选对应序号的选项。
        const int digit = key - Qt::Key_1;
        if (digit >= 0 && digit < 9 && digit < practiceController_.questionCount() &&
            practiceController_.currentQuestionIndex() >= 0) {
            if (digit < practiceController_.currentOptionCount()) {
                practiceController_.chooseAnswer(digit); return;
            }
        }
        if (key == Qt::Key_Enter || key == Qt::Key_Return) { practiceController_.submitAttempt(); return; }
    } else if (page == solutionPage_) {
        const int key = event->key();
        if (key == Qt::Key_Left || key == Qt::Key_Up) {
            solutionController_.showSolution(solutionController_.currentSolutionIndex() - 1); return;
        }
        if (key == Qt::Key_Right || key == Qt::Key_Down) {
            solutionController_.showSolution(solutionController_.currentSolutionIndex() + 1); return;
        }
    }
    QMainWindow::keyPressEvent(event);
}

void MainWindow::applyCardStyle() {
    const bool light = AppSettings::colorTheme() == QStringLiteral("light");
    const QString path = light
        ? QStringLiteral(":/styles/desktop-light.qss")
        : QStringLiteral(":/styles/desktop.qss");
    QFile style(path);
    if (!style.open(QIODevice::ReadOnly)) {
        qWarning("Unable to load embedded desktop stylesheet");
        return;
    }
    setStyleSheet(QString::fromUtf8(style.readAll()) +
                  backgroundVisibilityStyle(light, backgroundVisibility_));
}

}  // namespace quizpane
