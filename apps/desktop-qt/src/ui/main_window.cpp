#include "main_window.hpp"
#include "../app_settings.hpp"
#include "../platform/global_hotkey.hpp"
#include "../platform/window_pinning.hpp"
#include "app_dialogs.hpp"
#include "quizpane/diagnostic_logger.hpp"
#include "quizpane/provider_response_router.hpp"
#include "line_icons.hpp"
#include "material_card.hpp"
#include "question_navigator.hpp"

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
#include <QResizeEvent>
#include <QScreen>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QSysInfo>
#include <QStyle>
#include <QTextDocument>
#include <QTimer>
#include <QUrl>
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
    QTextDocument document;
    document.setHtml(html);
    return document.toPlainText().trimmed();
}

QString choiceLabel(int choice) {
    return choice >= 0 ? QString(QChar(u'A' + choice)) : QStringLiteral("未作答");
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
#if defined(Q_OS_WIN)
    return QStringLiteral("QuizPane-windows-x64-portable.zip");
#elif defined(Q_OS_MACOS)
    const QString arch = QSysInfo::currentCpuArchitecture().toLower();
    return arch.contains(QStringLiteral("arm"))
        ? QStringLiteral("QuizPane-macos-arm64.dmg")
        : QStringLiteral("QuizPane-macos-x86_64.dmg");
#else
    return {};
#endif
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

}  // namespace

// ===== 窗口与页面装配 =====

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    // Qt Widgets 通过父子对象树管理控件生命周期；窗口析构时会递归释放子控件。
    pinned_ = AppSettings::windowPinned();
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
    auto* catalogPageLayout = new QVBoxLayout(catalogPage_);
    catalogPageLayout->setContentsMargins(0, 14, 0, 0);
    auto* catalogTitle = new QLabel(QStringLiteral("选择练习"));
    catalogTitle->setObjectName(QStringLiteral("pageTitle"));
    auto* catalogHint = new QLabel(QStringLiteral("选择分类和题量，立即开始一组练习。"));
    catalogHint->setObjectName(QStringLiteral("detail"));
    auto* catalogScroll = new QScrollArea;
    catalogScroll->setWidgetResizable(true);
    catalogScroll->setFrameShape(QFrame::NoFrame);
    auto* catalogContent = new QWidget;
    catalogListLayout_ = new QVBoxLayout(catalogContent);
    catalogListLayout_->setContentsMargins(0, 4, 4, 4);
    catalogListLayout_->setSpacing(10);
    catalogListLayout_->addStretch();
    catalogScroll->setWidget(catalogContent);
    catalogPageLayout->addWidget(catalogTitle);
    catalogPageLayout->addWidget(catalogHint);
    catalogPageLayout->addWidget(catalogScroll, 1);

    practicePage_ = new QWidget;
    auto* practiceLayout = new QVBoxLayout(practicePage_);
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
    connect(previousQuestionButton_, &QPushButton::clicked, this,
            [this] { showQuestion(currentQuestionIndex_ - 1); });
    connect(nextQuestionButton_, &QPushButton::clicked, this,
            [this] { showQuestion(currentQuestionIndex_ + 1); });
    connect(questionListButton_, &QPushButton::clicked,
            this, &MainWindow::toggleQuestionNavigator);
    connect(submitButton_, &QPushButton::clicked, this, &MainWindow::submitAttempt);
    practiceLayout->addWidget(practiceTitleLabel_);
    practiceLayout->addWidget(practiceProgressLabel_);
    practiceLayout->addWidget(questionScroll_);
    practiceLayout->addWidget(practiceControlBar_);

    questionNavigator_ = new ui::QuestionNavigator(this);
    connect(questionNavigator_, &ui::QuestionNavigator::questionSelected,
            this, &MainWindow::showQuestion);

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
    connect(cancelSubmitButton, &QPushButton::clicked,
            this, &MainWindow::hideSubmitConfirmation);
    connect(confirmSubmitButton, &QPushButton::clicked,
            this, &MainWindow::confirmSubmitAttempt);

    solutionPage_ = new QWidget;
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
    connect(previousSolutionButton_, &QPushButton::clicked, this,
            [this] { showSolution(currentSolutionIndex_ - 1); });
    connect(nextSolutionButton_, &QPushButton::clicked, this,
            [this] { showSolution(currentSolutionIndex_ + 1); });
    connect(exportResultsButton_, &QPushButton::clicked, this,
            &MainWindow::exportAttemptResults);
    connect(backToCatalogButton, &QPushButton::clicked, this,
            [this] {
                pages_->setCurrentWidget(catalogPage_);
                applyUiSize(uiSize_);
            });
    solutionLayout->addWidget(resultSummaryLabel_);
    solutionLayout->addWidget(solutionProgressLabel_);
    solutionLayout->addWidget(solutionScroll, 1);
    solutionLayout->addWidget(solutionControlBar_);

    pages_->addWidget(loginPage_);
    pages_->addWidget(catalogPage_);
    pages_->addWidget(practicePage_);
    pages_->addWidget(solutionPage_);
    layout->addWidget(pages_, 1);
    resizeHandle_ = new VerticalResizeHandle(card_);
    resizeHandle_->setObjectName(QStringLiteral("verticalResizeHandle"));
    resizeHandle_->setVisible(false);
    layout->addWidget(resizeHandle_);
    connect(pages_, &QStackedWidget::currentChanged, this, [this] {
        if (questionNavigator_) questionNavigator_->hide();
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
    globalHotkey_ = new GlobalHotkey(this);
    connect(globalHotkey_, &GlobalHotkey::activated, this,
            &MainWindow::toggleWindowVisibility);
    const QString savedHotkey = AppSettings::bossKey();
    bossKey_ = QKeySequence::fromString(savedHotkey, QKeySequence::PortableText);
    if (bossKey_.isEmpty()) bossKey_ = QKeySequence(QStringLiteral("Ctrl+Shift+H"));
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
    trayMenu_->addAction(QStringLiteral("添加题库…"), this,
                         &MainWindow::chooseProviderPackage);
    trayMenu_->addAction(QStringLiteral("老板键设置…"), this,
                         &MainWindow::configureBossKey);
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

void MainWindow::toggleWindowVisibility() {
    if (visibilityToggleDebounce_.isValid() &&
        visibilityToggleDebounce_.elapsed() < 150) return;
    visibilityToggleDebounce_.restart();
    if (isVisible()) {
        hide();
        if (showHideAction_) showHideAction_->setText(QStringLiteral("显示窗口"));
        return;
    }
    show();
    platform::applyNativeWindowPin(this, pinned_);
    if (pinned_) raise();
    activateWindow();
    if (showHideAction_) showHideAction_->setText(QStringLiteral("隐藏窗口"));
}

// ===== 当前练习会话与草稿 =====

void MainWindow::returnToCatalog() {
    pages_->setCurrentWidget(catalogPage_);
    applyUiSize(uiSize_);
}

void MainWindow::saveDraft() {
    if (providerId_.isEmpty() || attemptId_.isEmpty() || questions_.isEmpty()) return;
    // UI 当前状态先组装为 DTO，再交给 Core 持久化；MainWindow 不关心文件路径和
    // 原子写入细节，这与 Controller -> Service 的分层方式一致。
    DraftSnapshot snapshot;
    snapshot.providerId = providerId_;
    snapshot.attemptId = attemptId_;
    snapshot.title = attemptTitle_;
    snapshot.questions = questions_;
    for (auto it = materialsById_.constBegin(); it != materialsById_.constEnd(); ++it)
        snapshot.materials.append(it.value());
    snapshot.answers = answers_;
    snapshot.currentQuestionIndex = currentQuestionIndex_;
    QString error;
    if (!draftStore_.save(snapshot, &error))
        qWarning("Unable to save draft: %s", qPrintable(error));
}

bool MainWindow::maybeRestoreDraft() {
    if (draftRestoreChecked_ || providerId_.isEmpty()) return false;
    draftRestoreChecked_ = true;
    QString error;
    QList<DraftSnapshot> drafts = draftStore_.list(providerId_, &error);
    while (!drafts.isEmpty()) {
        const ui::DraftDecision decision = ui::chooseDraft(this, drafts);
        if (decision.choice == ui::DraftChoice::Later) return false;
        if (decision.index < 0 || decision.index >= drafts.size()) return false;
        if (decision.choice == ui::DraftChoice::Discard) {
            draftStore_.clearAttempt(providerId_, drafts.at(decision.index).attemptId, &error);
            drafts.removeAt(decision.index);
            continue;
        }
        const DraftSnapshot snapshot = drafts.at(decision.index);
    attemptId_ = snapshot.attemptId;
    attemptTitle_ = snapshot.title;
    questions_ = snapshot.questions;
    updateMaterialsCache(snapshot.materials);
    answers_ = snapshot.answers;
    const int restoredAnswerCount = answers_.size();
    answers_.resize(static_cast<int>(questions_.size()));
    for (int index = restoredAnswerCount; index < answers_.size(); ++index)
        answers_[index] = -1;
    multiAnswers_.resize(answers_.size());
    lockedPracticeViewportHeight_ = 0;
    submitButton_->setEnabled(true);
    pages_->setCurrentWidget(practicePage_);
    showQuestion(qBound(0, snapshot.currentQuestionIndex,
                        static_cast<int>(questions_.size()) - 1));
        return true;
    }
    return false;
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
    if (!provider_.request({{"id", "auth-begin"}, {"method", "auth.begin"},
                            {"params", QJsonObject{}}}, &error))
        detailLabel_->setText(error);
}

void MainWindow::pollLogin() {
    if (loginSessionId_.isEmpty()) return;
    QString error;
    if (!provider_.request({{"id", "auth-poll"}, {"method", "auth.poll"},
                            {"params", QJsonObject{{"loginSessionId", loginSessionId_}}}},
                           &error)) {
        detailLabel_->setText(error);
        loginPollTimer_->start(2500);
    }
}

void MainWindow::requestCatalog() {
    pages_->setCurrentWidget(loginPage_);
    applyUiSize(uiSize_);
    titleLabel_->setText(QStringLiteral("正在加载练习分类…"));
    QString error;
    if (!provider_.request({{"id", "catalog-list"}, {"method", "catalog.list"},
                            {"params", QJsonObject{}}}, &error))
        detailLabel_->setText(error);
}

void MainWindow::clearLayout(QLayout* layout) {
    while (layout && layout->count() > 0) {
        QLayoutItem* item = layout->takeAt(0);
        if (item->layout()) clearLayout(item->layout());
        delete item->widget();
        delete item;
    }
}

void MainWindow::populateCatalog(const QJsonArray& nodes) {
    clearLayout(catalogListLayout_);
    for (const auto& value : nodes) {
        if (!value.isObject()) continue;
        const QJsonObject node = value.toObject();
        const QString categoryId = node.value("id").toString();
        const QString title = node.value("title").toString();
        const int available = node.value("availableQuestionCount").toInt();
        if (categoryId.isEmpty() || title.isEmpty()) continue;

        auto* row = new QWidget;
        row->setObjectName(QStringLiteral("catalogRow"));
        auto* rowLayout = new QVBoxLayout(row);
        rowLayout->setContentsMargins(12, 10, 10, 10);
        rowLayout->setSpacing(7);
        auto* label = new QLabel(QStringLiteral("%1\n%2 道可练").arg(title).arg(available));
        const int mastered = node.value("masteredCount").toInt();
        const int mistakes = node.value("mistakeCount").toInt();
        label->setText(QStringLiteral("%1\n%2 道可练 · 已掌握 %3 · 待巩固 %4")
            .arg(title).arg(available).arg(mastered).arg(mistakes));
        label->setWordWrap(true);
        rowLayout->addWidget(label);
        auto* includeAnswered = new QCheckBox(QStringLiteral("包含之前做过的题"));
        includeAnswered->setObjectName(QStringLiteral("includeAnswered"));
        includeAnswered->setChecked(false);
        rowLayout->addWidget(includeAnswered);
        QList<int> counts;
        for (const auto& countValue : node.value("suggestedCounts").toArray()) {
            const int count = countValue.toInt();
            if (count > 0 && count <= available && !counts.contains(count)) counts.append(count);
        }
        for (const int preset : {5, 10, 15, 20})
            if (preset <= available && !counts.contains(preset)) counts.append(preset);
        if (available > 0 && !counts.contains(available)) counts.append(available);
        std::sort(counts.begin(), counts.end());
        auto* countLayout = new QGridLayout;
        countLayout->setContentsMargins(0, 0, 0, 0);
        countLayout->setHorizontalSpacing(6);
        countLayout->setVerticalSpacing(6);
        int buttonIndex = 0;
        for (const int count : counts) {
            auto* button = new QPushButton(QStringLiteral("%1 题").arg(count));
            if (count == available) button->setText(QStringLiteral("全部 %1 题").arg(available));
            button->setObjectName(QStringLiteral("smallButton"));
            connect(button, &QPushButton::clicked, this,
                    [this, categoryId, title, count, includeAnswered] {
                        startAttempt(categoryId, title, count, includeAnswered->isChecked());
                    });
            countLayout->addWidget(button, buttonIndex / 3, buttonIndex % 3);
            ++buttonIndex;
        }
        rowLayout->addLayout(countLayout);
        catalogListLayout_->addWidget(row);
    }
    catalogListLayout_->addStretch();
    pages_->setCurrentWidget(catalogPage_);
    applyUiSize(uiSize_);
}

void MainWindow::startAttempt(const QString& categoryId, const QString& title,
                              int count, bool includePreviouslyAnswered) {
    attemptTitle_ = title;
    practiceProgressLabel_->setText(QStringLiteral("正在创建练习 · %1 题").arg(count));
    questionLabel_->setText(QStringLiteral("请稍候…"));
    clearLayout(optionsLayout_);
    lockedPracticeViewportHeight_ = 0;
    questionScroll_->setMinimumHeight(layout_metrics::kMinimumPracticeViewportHeight);
    pages_->setCurrentWidget(practicePage_);
    QString error;
    if (!provider_.request(
            {{"id", "attempt-create"}, {"method", "attempt.create"},
             {"params", QJsonObject{{"categoryId", categoryId}, {"count", count},
                                      {"includePreviouslyAnswered", includePreviouslyAnswered}}}},
            &error))
        QMessageBox::warning(this, QStringLiteral("无法创建练习"), error);
}

void MainWindow::requestQuestions() {
    QString error;
    if (!provider_.request(
            {{"id", "attempt-questions"}, {"method", "attempt.questions"},
             {"params", QJsonObject{{"attemptId", attemptId_}}}}, &error))
        QMessageBox::warning(this, QStringLiteral("无法读取题目"), error);
}

void MainWindow::updateMaterialsCache(const QJsonArray& materials) {
    // attempt.questions 和 attempt.solutions 各自只返回当前作答范围内实际
    // 用到的材料（见 DeclarativeProvider::hostMaterials），这里合并进同一份
    // 缓存而不是整体替换，避免答题阶段收到的材料在进入解析页后丢失。
    for (const auto& value : materials) {
        const QJsonObject material = value.toObject();
        const QString materialId = material.value("id").toString();
        if (!materialId.isEmpty()) materialsById_.insert(materialId, material);
    }
}

void MainWindow::showQuestion(int index) {
    if (questions_.isEmpty()) return;
    hideSubmitConfirmation();
    currentQuestionIndex_ = qBound(0, index, static_cast<int>(questions_.size()) - 1);
    const QJsonObject question = questions_.at(currentQuestionIndex_).toObject();
    practiceProgressLabel_->setText(QStringLiteral("第 %1 / %2 题 · 已作答 %3 题")
        .arg(currentQuestionIndex_ + 1).arg(questions_.size())
        .arg(std::count_if(answers_.cbegin(), answers_.cend(), [](int value) { return value >= 0; })));
    const QString materialId = question.value("materialId").toString();
    if (materialId.isEmpty()) {
        practiceMaterialCard_->hideMaterial();
    } else {
        const QJsonObject material = materialsById_.value(materialId);
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
    optionButtonGroup_ = multiple ? nullptr : new QButtonGroup(this);
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
        button->setChecked(multiple ? multiAnswers_.value(currentQuestionIndex_).contains(optionIndex)
                                    : answers_.value(currentQuestionIndex_, -1) == optionIndex);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        if (optionButtonGroup_)
            optionButtonGroup_->addButton(button);
        connect(button, &QAbstractButton::toggled, this,
                [this, optionIndex](bool checked) {
                    if (questions_.at(currentQuestionIndex_).toObject().value("type").toString() ==
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
        connect(button, &QAbstractButton::toggled, optionCard,
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
    nextQuestionButton_->setEnabled(currentQuestionIndex_ + 1 < questions_.size());
    questionScroll_->verticalScrollBar()->setValue(0);
    if (lockedPracticeViewportHeight_ == 0)
        QTimer::singleShot(0, this, &MainWindow::lockCompactPracticeHeight);
    refreshQuestionNavigator();
    saveDraft();
}

void MainWindow::chooseAnswer(int choice, bool checked) {
    if (currentQuestionIndex_ < 0 || currentQuestionIndex_ >= answers_.size()) return;
    const bool multiple = questions_.at(currentQuestionIndex_).toObject().value("type").toString() ==
        QStringLiteral("multiple_choice");
    if (multiple) {
        if (checked) multiAnswers_[currentQuestionIndex_].insert(choice);
        else multiAnswers_[currentQuestionIndex_].remove(choice);
        answers_[currentQuestionIndex_] = multiAnswers_[currentQuestionIndex_].isEmpty()
            ? -1 : *multiAnswers_[currentQuestionIndex_].constBegin();
    } else answers_[currentQuestionIndex_] = choice;
    practiceProgressLabel_->setText(QStringLiteral("第 %1 / %2 题 · 已作答 %3 题")
        .arg(currentQuestionIndex_ + 1).arg(questions_.size())
        .arg(std::count_if(answers_.cbegin(), answers_.cend(), [](int value) { return value >= 0; })));
    refreshQuestionNavigator();
    const QJsonObject question = questions_.at(currentQuestionIndex_).toObject();
    QJsonValue savedChoice = QString::number(choice);
    if (multiple) { QJsonArray values; for (int value : multiAnswers_[currentQuestionIndex_]) values.append(QString::number(value)); savedChoice = values; }
    const QJsonArray payload{QJsonObject{
        {"questionIndex", currentQuestionIndex_},
        {"questionId", question.value("id").toString()},
        {"time", 0}, {"flag", 0},
        {"answer", QJsonObject{{"type", multiple ? 202 : 201}, {"choice", savedChoice}}}}};
    QString error;
    // request 只发起异步 RPC，不阻塞 UI；结果统一进入 handleProviderResponse()。
    provider_.request(
        {{"id", QStringLiteral("save-%1-%2").arg(currentQuestionIndex_).arg(choice)},
         {"method", "attempt.saveAnswers"},
         {"params", QJsonObject{{"attemptId", attemptId_}, {"answers", payload}}}},
        &error);
    saveDraft();
    // 留出选中反馈再自动进入下一题。原值 120ms 几乎是“一眨眼”，在职备考用户
    // 常想先点一个答案、再核对是否改主意，根本来不及回退；默认放宽到 700ms
    // 并允许在设置里调整（practice/autoAdvanceMs），设为 0 即关闭自动跳题。
    // 若用户已手动切题，index 校验会阻止旧定时器把新页面再次向前推进。
    const int advanceMs = AppSettings::autoAdvanceMs();
    const int answeredIndex = currentQuestionIndex_;
    if (multiple || advanceMs <= 0) return;
    if (answeredIndex + 1 < questions_.size()) {
        QTimer::singleShot(advanceMs, this, [this, answeredIndex] {
            if (currentQuestionIndex_ == answeredIndex)
                showQuestion(answeredIndex + 1);
        });
    } else {
        // 最后一题没有“下一题”可跳，短暂停留显示选中反馈后直接询问是否交卷。
        QTimer::singleShot(advanceMs, this, [this, answeredIndex] {
            if (currentQuestionIndex_ == answeredIndex)
                submitAttempt();
        });
    }
}

void MainWindow::refreshQuestionNavigator() {
    if (!questionNavigator_) return;
    QSet<int> answered;
    for (int index = 0; index < answers_.size(); ++index) {
        if (answers_.at(index) >= 0) answered.insert(index);
    }
    questionNavigator_->setState(static_cast<int>(questions_.size()), answered,
                                 currentQuestionIndex_);
}

void MainWindow::toggleQuestionNavigator() {
    if (!questionNavigator_ || questions_.isEmpty()) return;
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

QJsonArray MainWindow::answerPayload() const {
    QJsonArray payload;
    for (qsizetype index = 0; index < questions_.size(); ++index) {
        const int selected = answers_.value(index, -1);
        const bool multiple = questions_.at(index).toObject().value("type").toString() == QStringLiteral("multiple_choice");
        QJsonValue choice = selected >= 0 ? QJsonValue(QString::number(selected)) : QJsonValue(QJsonValue::Null);
        if (multiple) { QJsonArray selections; for (int value : multiAnswers_.value(index)) selections.append(QString::number(value)); choice = selections; }
        payload.append(QJsonObject{
            {"questionIndex", index},
            {"questionId", questions_.at(index).toObject().value("id").toString()},
            {"time", 0}, {"flag", 0},
            {"answer", QJsonObject{{"type", multiple ? 202 : 201}, {"choice", choice}}}});
    }
    return payload;
}

void MainWindow::submitAttempt() {
    const int answered = static_cast<int>(std::count_if(
        answers_.cbegin(), answers_.cend(), [](int value) { return value >= 0; }));
    const int unanswered = answers_.size() - answered;
    submitConfirmationLabel_->setText(unanswered > 0
        ? QStringLiteral("还有 %1 题未作答，确定交卷吗？").arg(unanswered)
        : QStringLiteral("已完成全部题目，确定交卷吗？"));
    submitConfirmationBubble_->adjustSize();
    positionSubmitConfirmation();
    submitConfirmationBubble_->show();
    submitConfirmationBubble_->raise();
}

void MainWindow::hideSubmitConfirmation() {
    if (submitConfirmationBubble_) submitConfirmationBubble_->hide();
}

void MainWindow::positionSubmitConfirmation() {
    if (!submitConfirmationBubble_ || !practiceControlBar_) return;
    const int x = qMax(4, practicePage_->width() -
                             submitConfirmationBubble_->width() - 4);
    const int y = qMax(4, practiceControlBar_->y() -
                             submitConfirmationBubble_->height() -
                                 layout_metrics::kSubmitBubbleInset);
    submitConfirmationBubble_->move(x, y);
}

void MainWindow::confirmSubmitAttempt() {
    hideSubmitConfirmation();
    submitButton_->setEnabled(false);
    practiceProgressLabel_->setText(QStringLiteral("正在保存答案…"));
    QString error;
    if (!provider_.request(
            {{"id", "final-save"}, {"method", "attempt.saveAnswers"},
             {"params", QJsonObject{{"attemptId", attemptId_},
                                     {"answers", answerPayload()}}}}, &error)) {
        submitButton_->setEnabled(true);
        QMessageBox::warning(this, QStringLiteral("保存失败"), error);
    }
}

void MainWindow::sendSubmit() {
    practiceProgressLabel_->setText(QStringLiteral("正在交卷…"));
    QString error;
    if (!provider_.request(
            {{"id", "attempt-submit"}, {"method", "attempt.submit"},
             {"params", QJsonObject{{"attemptId", attemptId_}}}}, &error)) {
        submitButton_->setEnabled(true);
        QMessageBox::warning(this, QStringLiteral("交卷失败"), error);
    }
}

void MainWindow::requestResults() {
    resultSummaryLabel_->setText(QStringLiteral("正在生成答题结果…"));
    pages_->setCurrentWidget(solutionPage_);
    applyUiSize(uiSize_);
    QString error;
    provider_.request({{"id", "attempt-report"}, {"method", "attempt.report"},
                       {"params", QJsonObject{{"attemptId", attemptId_}}}}, &error);
    if (attemptHasAnswerKey_)
        provider_.request({{"id", "attempt-solutions"}, {"method", "attempt.solutions"},
                           {"params", QJsonObject{{"attemptId", attemptId_}}}}, &error);
}

void MainWindow::showSolution(int index) {
    if (solutions_.isEmpty()) return;
    currentSolutionIndex_ = qBound(0, index, static_cast<int>(solutions_.size()) - 1);
    const QJsonObject solution = solutions_.at(currentSolutionIndex_).toObject();
    const QString materialId = solution.value("materialId").toString();
    if (materialId.isEmpty()) {
        solutionMaterialCard_->hideMaterial();
    } else {
        const QJsonObject material = materialsById_.value(materialId);
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
    const int selected = answers_.value(currentSolutionIndex_, -1);
    const auto labels = [](const QSet<int>& set) { QStringList result; for (int value : set) result.append(choiceLabel(value)); std::sort(result.begin(), result.end()); return result.join(QStringLiteral("、")); };
    const QSet<int> selectedChoices = multiple ? multiAnswers_.value(currentSolutionIndex_) : QSet<int>{selected};
    selectedAnswerLabel_->setText(QStringLiteral("你的答案\n%1").arg(multiple ? labels(selectedChoices) : choiceLabel(selected)));
    if (attemptHasAnswerKey_) {
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
        .arg(currentSolutionIndex_ + 1).arg(solutions_.size()));
    previousSolutionButton_->setEnabled(currentSolutionIndex_ > 0);
    nextSolutionButton_->setEnabled(currentSolutionIndex_ + 1 < solutions_.size());
}

void MainWindow::exportAttemptResults() {
    if (questions_.isEmpty())
        return;
    constexpr int imageWidth = 1080;
    constexpr int padding = 44;
    constexpr int columns = 4;
    constexpr int rowHeight = 74;
    constexpr int maxRowsPerImage = 72;
    constexpr int headerHeight = 208;
    constexpr int gap = 14;
    const int itemCapacity = columns * maxRowsPerImage;
    const int pageCount = (questions_.size() + itemCapacity - 1) / itemCapacity;
    QList<QImage> resultImages;
    const auto answerText = [this](qsizetype index, const QJsonObject& question) {
        if (question.value("type").toString() == QStringLiteral("multiple_choice")) {
            QStringList choices;
            for (const int choice : multiAnswers_.value(index)) choices.append(choiceLabel(choice));
            std::sort(choices.begin(), choices.end());
            return choices.isEmpty() ? QStringLiteral("未作答") : choices.join(QStringLiteral("、"));
        }
        return choiceLabel(answers_.value(index, -1));
    };
    for (int page = 0; page < pageCount; ++page) {
        const int first = page * itemCapacity;
        const int count = qMin(itemCapacity, int(questions_.size()) - first);
        const int rows = (count + columns - 1) / columns;
        QImage image(imageWidth, headerHeight + rows * rowHeight + padding,
                     QImage::Format_ARGB32_Premultiplied);
        image.fill(QColor(QStringLiteral("#10151c")));
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        QFont titleFont = painter.font();
        titleFont.setPixelSize(38);
        titleFont.setBold(true);
        painter.setFont(titleFont);
        painter.setPen(QColor(QStringLiteral("#f2f5f8")));
        painter.drawText(padding, 66, QStringLiteral("作答结果"));
        QFont detailFont = painter.font();
        detailFont.setPixelSize(22);
        detailFont.setBold(false);
        painter.setFont(detailFont);
        painter.setPen(QColor(QStringLiteral("#aeb8c3")));
        painter.drawText(padding, 104, attemptTitle_.isEmpty() ? QStringLiteral("题库练习") : attemptTitle_);
        const int answered = std::count_if(answers_.cbegin(), answers_.cend(),
            [](int answer) { return answer >= 0; });
        painter.drawText(padding, 140, QStringLiteral("已作答 %1 / %2 题 · %3")
            .arg(answered).arg(questions_.size())
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"))));
        painter.drawText(padding, 174, attemptHasAnswerKey_
            ? QStringLiteral("仅汇总你的选择，便于快速对照答案")
            : QStringLiteral("无答案题库：仅记录你的选择，不提供判分"));
        if (pageCount > 1)
            painter.drawText(imageWidth - padding - 90, 174,
                             QStringLiteral("%1 / %2").arg(page + 1).arg(pageCount));
        const qreal cardWidth = (imageWidth - padding * 2 - gap * (columns - 1)) / qreal(columns);
        for (int offset = 0; offset < count; ++offset) {
            const int index = first + offset;
            const QJsonObject question = questions_.at(index).toObject();
            const int column = offset % columns;
            const int row = offset / columns;
            const QRectF card(padding + column * (cardWidth + gap), headerHeight + row * rowHeight,
                              cardWidth, rowHeight - gap);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(QStringLiteral("#1b232d")));
            painter.drawRoundedRect(card, 12, 12);
            const int sourceNumber = question.value("sourceQuestionNumber").toInt(index + 1);
            QFont numberFont = painter.font();
            numberFont.setPixelSize(22);
            numberFont.setBold(true);
            painter.setFont(numberFont);
            painter.setPen(QColor(QStringLiteral("#e7edf4")));
            painter.drawText(card.adjusted(16, 8, -16, -8), Qt::AlignLeft | Qt::AlignTop,
                             QStringLiteral("%1").arg(sourceNumber));
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
        resultImages.append(image);
    }
    if (resultImages.isEmpty())
        return;

    QDialog dialog(this);
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
    const auto showImage = [&] {
        preview->setPixmap(QPixmap::fromImage(resultImages.at(currentImage)));
        pageLabel->setText(QStringLiteral("%1 / %2").arg(currentImage + 1).arg(resultImages.size()));
        previous->setEnabled(currentImage > 0);
        next->setEnabled(currentImage + 1 < resultImages.size());
    };
    connect(previous, &QPushButton::clicked, &dialog, [&] {
        if (currentImage > 0) { --currentImage; showImage(); }
    });
    connect(next, &QPushButton::clicked, &dialog, [&] {
        if (currentImage + 1 < resultImages.size()) { ++currentImage; showImage(); }
    });
    connect(save, &QPushButton::clicked, &dialog, [&] {
        const QString suffix = resultImages.size() > 1
            ? QStringLiteral("-%1").arg(currentImage + 1) : QString();
        const QString suggested = QDir(QStandardPaths::writableLocation(
            QStandardPaths::PicturesLocation)).filePath(QStringLiteral("作答结果%1.png").arg(suffix));
        const QString path = QFileDialog::getSaveFileName(&dialog, QStringLiteral("保存作答结果"),
            suggested, QStringLiteral("PNG 图片 (*.png)"));
        if (!path.isEmpty() && !resultImages.at(currentImage).save(path, "PNG"))
            QMessageBox::warning(&dialog, QStringLiteral("保存失败"),
                                 QStringLiteral("无法写入图片：%1").arg(path));
    });
    showImage();
    dialog.exec();
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
    // 这是桌面端的“响应路由器”。Provider 回包携带请求 id，我们据此更新对应页面。
    // 请求 id 决定路由；同一时刻可并行等待报告和题目解析。
    const ProviderResponseEnvelope envelope = routeProviderResponse(response);
    const QString& id = envelope.id;
    diagnostic::event(QStringLiteral("ui"), QStringLiteral("provider-response-route"),
        {{QStringLiteral("id"), id},
         {QStringLiteral("error"), response.contains(QStringLiteral("error"))}});
    if (envelope.failed) {
        const QString message = userFacingError(envelope.error);
        detailLabel_->setText(message);
        if (id == QStringLiteral("auth-begin")) {
            titleLabel_->setText(QStringLiteral("二维码生成失败"));
            setActionMode(ActionMode::ConnectAccount, QStringLiteral("重新尝试"));
            return;
        }
        if (id == QStringLiteral("auth-poll") && loginSessionId_.isEmpty()) return;
        if (id == QStringLiteral("auth-poll") && !loginSessionId_.isEmpty())
            loginPollTimer_->start(3000);
        else if (id == QStringLiteral("catalog-list")) {
            pages_->setCurrentWidget(loginPage_);
            titleLabel_->setText(QStringLiteral("登录状态已失效"));
            detailLabel_->setText(QStringLiteral("请重新使用对应题库 App 扫码连接账号。"));
            setActionMode(ActionMode::ConnectAccount, QStringLiteral("重新连接"));
        }
        else if (!id.startsWith(QStringLiteral("save-")))
            QMessageBox::warning(this, QStringLiteral("操作失败"), message);
        return;
    }
    const QJsonObject& result = envelope.result;
    if (id == QStringLiteral("host-init-1")) {
        if (result.value("requiresLogin").toBool() &&
            !result.value("sessionRestored").toBool()) {
            titleLabel_->setText(QStringLiteral("题库账号尚未连接"));
            detailLabel_->setText(QStringLiteral("点击下方按钮，使用对应题库 App 扫码登录。"));
            setActionMode(ActionMode::ConnectAccount, QStringLiteral("连接题库账号"));
        } else {
            if (!maybeRestoreDraft()) requestCatalog();
        }
    } else if (id == QStringLiteral("auth-begin")) {
        loginSessionId_ = result.value("loginSessionId").toString();
        setQrContent(result.value("qrContent").toString());
        titleLabel_->setText(QStringLiteral("请使用对应题库 App 扫码"));
        detailLabel_->setText(QStringLiteral("扫码后请在手机上确认登录。"));
        setActionMode(ActionMode::CancelLogin, QStringLiteral("取消登录"));
        loginPollTimer_->start(result.value("pollIntervalMs").toInt(2000));
    } else if (id == QStringLiteral("auth-poll")) {
        const QString status = result.value("status").toString();
        if (status == QStringLiteral("authenticated")) {
            loginSessionId_.clear();
            qrLabel_->setVisible(false);
            titleLabel_->setText(QStringLiteral("登录成功"));
            detailLabel_->setText(QStringLiteral("正在加载练习分类…"));
            setActionMode(ActionMode::OpenCatalog, QStringLiteral("进入练习"));
            if (!maybeRestoreDraft()) requestCatalog();
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
    } else if (id == QStringLiteral("catalog-list")) {
        populateCatalog(result.value("nodes").toArray());
        setActionMode(ActionMode::OpenCatalog, QStringLiteral("刷新分类"));
    } else if (id == QStringLiteral("attempt-create")) {
        attemptId_ = result.value("attemptId").toString();
        attemptTitle_ = result.value("title").toString(attemptTitle_);
        attemptHasAnswerKey_ = result.value("hasAnswerKey").toBool(true);
        if (attemptId_.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("创建失败"),
                                 QStringLiteral("题库没有返回练习编号"));
            pages_->setCurrentWidget(catalogPage_);
            return;
        }
        requestQuestions();
    } else if (id == QStringLiteral("attempt-questions")) {
        questions_ = result.value("questions").toArray();
        updateMaterialsCache(result.value("materials").toArray());
        if (questions_.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("读取失败"),
                                 QStringLiteral("这套练习没有可显示的题目"));
            pages_->setCurrentWidget(catalogPage_);
            return;
        }
        answers_.fill(-1, static_cast<int>(questions_.size()));
        multiAnswers_.clear();
        multiAnswers_.resize(answers_.size());
        submitButton_->setEnabled(true);
        showQuestion(0);
        pages_->setCurrentWidget(practicePage_);
        saveDraft();
    } else if (id == QStringLiteral("final-save")) {
        sendSubmit();
    } else if (id == QStringLiteral("attempt-submit")) {
        draftStore_.clearAttempt(providerId_, attemptId_);
        requestResults();
    } else if (id == QStringLiteral("attempt-report")) {
        attemptHasAnswerKey_ = result.value("hasAnswerKey").toBool(attemptHasAnswerKey_);
        if (attemptHasAnswerKey_) {
            resultSummaryLabel_->setText(QStringLiteral("%1 / %2 题正确")
                .arg(result.value("correctCount").toInt())
                .arg(result.value("questionCount").toInt()));
        } else {
            resultSummaryLabel_->setText(QStringLiteral("已作答 %1 / %2 题 · 未提供答案，不评分")
                .arg(result.value("answerCount").toInt())
                .arg(result.value("questionCount").toInt()));
            solutions_ = questions_;
            if (!solutions_.isEmpty()) {
                showSolution(0);
                pages_->setCurrentWidget(solutionPage_);
            }
        }
    } else if (id == QStringLiteral("attempt-solutions")) {
        solutions_ = result.value("solutions").toArray();
        updateMaterialsCache(result.value("materials").toArray());
        if (solutions_.isEmpty()) {
            solutionProgressLabel_->setText(QStringLiteral("暂无题目解析"));
            return;
        }
        showSolution(0);
        pages_->setCurrentWidget(solutionPage_);
    }
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
    if (event->button() == Qt::LeftButton)
        dragOffset_ = event->globalPosition().toPoint() - frameGeometry().topLeft();
    QMainWindow::mousePressEvent(event);
}

void MainWindow::mouseMoveEvent(QMouseEvent* event) {
    if (event->buttons().testFlag(Qt::LeftButton) && !dragOffset_.isNull()) {
        move(event->globalPosition().toPoint() - dragOffset_);
        event->accept();
        return;
    }
    QMainWindow::mouseMoveEvent(event);
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    if (submitConfirmationBubble_ && submitConfirmationBubble_->isVisible())
        positionSubmitConfirmation();
    if (!pages_ || pages_->currentWidget() != practicePage_ ||
        questions_.isEmpty() || !questionScroll_) return;
    const auto* root = qobject_cast<QVBoxLayout*>(card_->layout());
    const auto* practice = qobject_cast<QVBoxLayout*>(practicePage_->layout());
    if (!root || !practice) return;
    const QMargins rootMargins = root->contentsMargins();
    const QMargins pageMargins = practice->contentsMargins();
    const int fixedHeight = rootMargins.top() + rootMargins.bottom() +
        headerBar_->height() + resizeHandle_->height() + root->spacing() * 2 +
        pageMargins.top() + pageMargins.bottom() +
        practiceProgressLabel_->sizeHint().height() +
        practiceControlBar_->height() + practice->spacing() * 2;
    const int viewportHeight = qMax(layout_metrics::kMinimumPracticeViewportHeight,
                                    event->size().height() - fixedHeight);
    lockedPracticeViewportHeight_ = viewportHeight;
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
    attemptId_.clear();
    questions_ = {};
    solutions_ = {};
    attemptHasAnswerKey_ = true;
    materialsById_.clear();
    practiceMaterialCard_->hideMaterial();
    solutionMaterialCard_->hideMaterial();
    answers_.clear();
    multiAnswers_.clear();
    if (questionNavigator_) questionNavigator_->hide();
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
    // 或依赖组合加载；“没有题库”绝不能因此呈现为加载失败。
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
        {{"id", "host-init-1"},
         {"method", "provider.initialize"},
         {"params", QJsonObject{{"hostVersion", "0.1.0"},
                                {"providerAbi", 1},
                                {"locale", "zh-CN"}}}},
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
    menu.addAction(QStringLiteral("老板键设置…"), this,
                   &MainWindow::configureBossKey);
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
    candidates << QDir(appDir).filePath(QStringLiteral("题库制作器.exe"));
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

    QNetworkRequest request(QUrl(QString::fromLatin1(kReleaseMetadataUrl)));
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
    const QString scriptPath = QDir(workDirectory).filePath(
#if defined(Q_OS_WIN)
        QStringLiteral("apply-update.ps1"));
    const QString destination = QCoreApplication::applicationDirPath();
    const QString restartPath = QCoreApplication::applicationFilePath();
    const QString script = QStringLiteral(R"PS(
param([string]$Package, [string]$Destination, [string]$Restart, [string]$Log)
$ErrorActionPreference = 'Stop'
try {
  Start-Sleep -Seconds 2
  $work = Join-Path (Split-Path -Parent $Package) 'expanded'
  Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
  Expand-Archive -LiteralPath $Package -DestinationPath $work -Force
  $source = Join-Path $work 'QuizPane'
  if (-not (Test-Path -LiteralPath (Join-Path $source '小窗刷题.exe'))) { throw '更新包结构不正确' }
  Get-ChildItem -LiteralPath $source -Force | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $Destination -Recurse -Force
  }
  Start-Process -FilePath $Restart
} catch {
  $_ | Out-File -LiteralPath $Log -Append -Encoding utf8
}
)PS");
    QFile scriptFile(scriptPath);
    if (!scriptFile.open(QIODevice::WriteOnly | QIODevice::Text) ||
        scriptFile.write(script.toUtf8()) < 0) {
        QMessageBox::warning(this, QStringLiteral("无法安装更新"),
            QStringLiteral("无法创建更新脚本。"));
        return false;
    }
    scriptFile.close();
    const QString powershell = QStandardPaths::findExecutable(QStringLiteral("powershell.exe"));
    if (powershell.isEmpty() || !QProcess::startDetached(powershell,
            {QStringLiteral("-NoProfile"), QStringLiteral("-WindowStyle"),
             QStringLiteral("Hidden"), QStringLiteral("-ExecutionPolicy"),
             QStringLiteral("Bypass"), QStringLiteral("-File"), scriptPath,
             updateDownloadPath_, destination, restartPath, updateScriptLogPath()})) {
        QMessageBox::warning(this, QStringLiteral("无法安装更新"),
            QStringLiteral("无法启动 Windows 更新程序。"));
        return false;
    }
    return true;
#elif defined(Q_OS_MACOS)
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
    Q_UNUSED(scriptPath)
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
    practiceControlBar_->setFixedHeight(layout_metrics::kControlBarHeight);
    solutionControlBar_->setFixedHeight(layout_metrics::kControlBarHeight);
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
    if (pages_->currentWidget() == practicePage_ && !questions_.isEmpty()) {
        lockedPracticeViewportHeight_ = 0;
        resize(windowSize.width(), height());
        if (isVisible()) move(oldTopRight.x() - width() + 1, oldTopRight.y());
        QTimer::singleShot(0, this, &MainWindow::lockCompactPracticeHeight);
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

int MainWindow::answerViewportMaximumHeight() const {
    if (uiSize_ == UiSize::Small) return 260;
    if (uiSize_ == UiSize::Large) return 480;
    return 360;
}

void MainWindow::lockCompactPracticeHeight() {
    if (pages_->currentWidget() != practicePage_ || questions_.isEmpty()) return;
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
        headerBar_->height() + resizeHandle_->height() + root->spacing() * 2 +
        pageMargins.top() + pageMargins.bottom() +
        practiceProgressLabel_->sizeHint().height() +
        practiceControlBar_->height() + practice->spacing() * 2;
    const int totalHeight = fixedHeight + lockedPracticeViewportHeight_;
    const QPoint oldTopRight = frameGeometry().topRight();
    resize(width(), totalHeight);
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
            showQuestion(currentQuestionIndex_ - 1); return;
        }
        if (key == Qt::Key_Right || key == Qt::Key_Down) {
            showQuestion(currentQuestionIndex_ + 1); return;
        }
        // 数字键 1-9 / 小键盘 1-9：直接选对应序号的选项。
        const int digit = key - Qt::Key_1;
        if (digit >= 0 && digit < 9 && digit < questions_.size() &&
            currentQuestionIndex_ >= 0 && currentQuestionIndex_ < questions_.size()) {
            const QJsonObject question = questions_.at(currentQuestionIndex_).toObject();
            if (digit < question.value("options").toArray().size()) { chooseAnswer(digit); return; }
        }
        if (key == Qt::Key_Enter || key == Qt::Key_Return) { submitAttempt(); return; }
    } else if (page == solutionPage_) {
        const int key = event->key();
        if (key == Qt::Key_Left || key == Qt::Key_Up) { showSolution(currentSolutionIndex_ - 1); return; }
        if (key == Qt::Key_Right || key == Qt::Key_Down) { showSolution(currentSolutionIndex_ + 1); return; }
    }
    QMainWindow::keyPressEvent(event);
}

void MainWindow::applyCardStyle() {
    const QString path = AppSettings::colorTheme() == QStringLiteral("light")
        ? QStringLiteral(":/styles/desktop-light.qss")
        : QStringLiteral(":/styles/desktop.qss");
    QFile style(path);
    if (!style.open(QIODevice::ReadOnly)) {
        qWarning("Unable to load embedded desktop stylesheet");
        return;
    }
    setStyleSheet(QString::fromUtf8(style.readAll()));
}

}  // namespace quizpane
