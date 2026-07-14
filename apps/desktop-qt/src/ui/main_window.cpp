#include "main_window.hpp"
#include "../platform/global_hotkey.hpp"
#include "../platform/window_pinning.hpp"
#include "app_dialogs.hpp"
#include "quizpane/diagnostic_logger.hpp"
#include "line_icons.hpp"
#include "material_card.hpp"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QImage>
#include <QIcon>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QMouseEvent>
#include <QPushButton>
#include <QPixmap>
#include <QProcess>
#include <QRadioButton>
#include <QResizeEvent>
#include <QScreen>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QStyle>
#include <QTextDocument>
#include <QTimer>
#include <QUrl>
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

QString userFacingError(QString message) {
    if (message.compare(QStringLiteral("Connection closed"),
                        Qt::CaseInsensitive) == 0 ||
        message.contains(QStringLiteral("remote host closed"),
                         Qt::CaseInsensitive))
        return QStringLiteral("网络连接中断，请稍后重试");
    if (message.contains(QStringLiteral("timed out"), Qt::CaseInsensitive))
        return QStringLiteral("网络请求超时，请检查网络后重试");
    if (message.contains(QStringLiteral("host not found"), Qt::CaseInsensitive))
        return QStringLiteral("无法连接题库服务，请检查网络");
    return message;
}

}  // namespace

// ===== 窗口与页面装配 =====

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    // Qt Widgets 采用“对象树”管理生命周期：把 card_、按钮、Label 的 parent
    // 指向窗口后，窗口析构时会递归释放它们。这里的 new 不等于 Java 中必然泄漏，
    // 也不需要逐个 delete；没有 parent 的普通 C++ 对象才需要 RAII/智能指针。
    pinned_ = QSettings().value(QStringLiteral("window/pinned"), true).toBool();
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
    // connect 相当于 jQuery 的事件绑定。信号带 bool 参数，槽函数签名在编译时校验。
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

    loginLayout->addWidget(titleLabel_);
    loginLayout->addWidget(qrLabel_);
    loginLayout->setAlignment(qrLabel_, Qt::AlignHCenter);
    loginLayout->addWidget(detailLabel_);
    loginLayout->addWidget(actionButton_);
    connect(actionButton_, &QPushButton::clicked, this,
            &MainWindow::runPrimaryAction);

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
    submitButton_ = new QPushButton;
    previousQuestionButton_->setIcon(makeLineIcon(LineIcon::Previous));
    nextQuestionButton_->setIcon(makeLineIcon(LineIcon::Next));
    submitButton_->setIcon(makeLineIcon(LineIcon::Submit));
    for (auto* button : {previousQuestionButton_, nextQuestionButton_, submitButton_})
        button->setObjectName(QStringLiteral("navIconButton"));
    previousQuestionButton_->setAccessibleName(QStringLiteral("上一题"));
    previousQuestionButton_->setToolTip(QStringLiteral("上一题"));
    nextQuestionButton_->setAccessibleName(QStringLiteral("下一题"));
    nextQuestionButton_->setToolTip(QStringLiteral("下一题"));
    submitButton_->setAccessibleName(QStringLiteral("交卷"));
    submitButton_->setToolTip(QStringLiteral("交卷"));
    questionNav->addWidget(previousQuestionButton_);
    questionNav->addWidget(nextQuestionButton_);
    questionNav->addStretch();
    questionNav->addWidget(submitButton_);
    connect(previousQuestionButton_, &QPushButton::clicked, this,
            [this] { showQuestion(currentQuestionIndex_ - 1); });
    connect(nextQuestionButton_, &QPushButton::clicked, this,
            [this] { showQuestion(currentQuestionIndex_ + 1); });
    connect(submitButton_, &QPushButton::clicked, this, &MainWindow::submitAttempt);
    practiceLayout->addWidget(practiceTitleLabel_);
    practiceLayout->addWidget(practiceProgressLabel_);
    practiceLayout->addWidget(questionScroll_);
    practiceLayout->addWidget(practiceControlBar_);

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
    solutionAnswerLabel_->setWordWrap(true);
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
    solutionNav->addWidget(backToCatalogButton);
    connect(previousSolutionButton_, &QPushButton::clicked, this,
            [this] { showSolution(currentSolutionIndex_ - 1); });
    connect(nextSolutionButton_, &QPushButton::clicked, this,
            [this] { showSolution(currentSolutionIndex_ + 1); });
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
        resizeHandle_->setVisible(pages_->currentWidget() == practicePage_);
        QTimer::singleShot(0, this, &MainWindow::adjustWindowForCurrentPage);
    });
    loginPollTimer_ = new QTimer(this);
    loginPollTimer_->setSingleShot(true);
    connect(loginPollTimer_, &QTimer::timeout, this, &MainWindow::pollLogin);
    setCentralWidget(card_);
    applyCardStyle();
    QSettings settings;
    QString savedSize = settings.value(QStringLiteral("ui/size")).toString();
    if (savedSize.isEmpty()) {
        QSettings legacySettings(QStringLiteral("QuizPane Project"),
                                 QStringLiteral("QuizPane"));
        savedSize = legacySettings.value(QStringLiteral("ui/size"),
                                         QStringLiteral("medium")).toString();
    }
    applyUiSize(savedSize == QStringLiteral("small") ? UiSize::Small
                : savedSize == QStringLiteral("large") ? UiSize::Large
                                                         : UiSize::Medium);
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
    const QString savedHotkey = QSettings().value(
        QStringLiteral("bossKey/sequence"), QStringLiteral("Ctrl+Shift+H"))
                                    .toString();
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
    trayMenu_->addAction(QStringLiteral("返回练习列表"), this,
                         &MainWindow::returnToCatalog);
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
    DraftSnapshot snapshot;
    QString error;
    if (!draftStore_.load(providerId_, &snapshot, &error)) return false;
    if (QMessageBox::question(this, QStringLiteral("恢复未完成练习"),
            QStringLiteral("发现一套未完成练习，是否恢复到上次答题位置？")) !=
        QMessageBox::Yes) return false;
    attemptId_ = snapshot.attemptId;
    attemptTitle_ = snapshot.title;
    questions_ = snapshot.questions;
    updateMaterialsCache(snapshot.materials);
    answers_ = snapshot.answers;
    const int restoredAnswerCount = answers_.size();
    answers_.resize(static_cast<int>(questions_.size()));
    for (int index = restoredAnswerCount; index < answers_.size(); ++index)
        answers_[index] = -1;
    lockedPracticeViewportHeight_ = 0;
    submitButton_->setEnabled(true);
    pages_->setCurrentWidget(practicePage_);
    showQuestion(qBound(0, snapshot.currentQuestionIndex,
                        static_cast<int>(questions_.size()) - 1));
    return true;
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
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(12, 10, 10, 10);
        auto* label = new QLabel(QStringLiteral("%1\n%2 道可练").arg(title).arg(available));
        label->setWordWrap(true);
        rowLayout->addWidget(label, 1);
        QList<int> counts;
        for (const auto& countValue : node.value("suggestedCounts").toArray()) {
            const int count = countValue.toInt();
            if (count > 0 && count <= available && !counts.contains(count)) counts.append(count);
        }
        if (counts.isEmpty()) {
            if (available >= 5) counts.append(5);
            if (available >= 15) counts.append(15);
            if (counts.isEmpty() && available > 0) counts.append(available);
        }
        for (const int count : counts.mid(0, 2)) {
            auto* button = new QPushButton(QStringLiteral("%1 题").arg(count));
            button->setObjectName(QStringLiteral("smallButton"));
            connect(button, &QPushButton::clicked, this,
                    [this, categoryId, title, count] { startAttempt(categoryId, title, count); });
            rowLayout->addWidget(button);
        }
        catalogListLayout_->addWidget(row);
    }
    catalogListLayout_->addStretch();
    pages_->setCurrentWidget(catalogPage_);
    applyUiSize(uiSize_);
}

void MainWindow::startAttempt(const QString& categoryId, const QString& title,
                              int count) {
    draftStore_.clear(providerId_);
    attemptTitle_ = title;
    practiceProgressLabel_->setText(QStringLiteral("正在创建练习 · %1 题").arg(count));
    questionLabel_->setText(QStringLiteral("请稍候…"));
    clearLayout(optionsLayout_);
    lockedPracticeViewportHeight_ = 0;
    questionScroll_->setFixedHeight(qMin(180, answerViewportMaximumHeight()));
    pages_->setCurrentWidget(practicePage_);
    QString error;
    if (!provider_.request(
            {{"id", "attempt-create"}, {"method", "attempt.create"},
             {"params", QJsonObject{{"categoryId", categoryId}, {"count", count}}}},
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
                                            material.value("contentHtml").toString());
    }
    questionLabel_->setText(QStringLiteral("<div style=\"color:#d5d1c5\">%1</div>")
        .arg(question.value("contentHtml").toString()));
    clearLayout(optionsLayout_);
    const QJsonArray options = question.value("options").toArray();
    for (qsizetype optionIndex = 0; optionIndex < options.size(); ++optionIndex) {
        const QJsonObject option = options.at(optionIndex).toObject();
        auto* button = new QRadioButton(QStringLiteral("%1. %2")
            .arg(option.value("label").toString(), plainText(option.value("contentHtml").toString())));
        button->setObjectName(QStringLiteral("answerOption"));
        button->setChecked(answers_.value(currentQuestionIndex_, -1) == optionIndex);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        connect(button, &QRadioButton::toggled, this,
                [this, optionIndex](bool checked) {
                    if (checked) chooseAnswer(static_cast<int>(optionIndex));
                });
        optionsLayout_->addWidget(button);
    }
    previousQuestionButton_->setEnabled(currentQuestionIndex_ > 0);
    nextQuestionButton_->setEnabled(currentQuestionIndex_ + 1 < questions_.size());
    questionScroll_->verticalScrollBar()->setValue(0);
    if (lockedPracticeViewportHeight_ == 0)
        QTimer::singleShot(0, this, &MainWindow::lockCompactPracticeHeight);
    saveDraft();
}

void MainWindow::chooseAnswer(int choice) {
    if (currentQuestionIndex_ < 0 || currentQuestionIndex_ >= answers_.size()) return;
    answers_[currentQuestionIndex_] = choice;
    practiceProgressLabel_->setText(QStringLiteral("第 %1 / %2 题 · 已作答 %3 题")
        .arg(currentQuestionIndex_ + 1).arg(questions_.size())
        .arg(std::count_if(answers_.cbegin(), answers_.cend(), [](int value) { return value >= 0; })));
    const QJsonObject question = questions_.at(currentQuestionIndex_).toObject();
    const QJsonArray payload{QJsonObject{
        {"questionIndex", currentQuestionIndex_},
        {"questionId", question.value("id").toString()},
        {"time", 0}, {"flag", 0},
        {"answer", QJsonObject{{"type", 201}, {"choice", QString::number(choice)}}}}};
    QString error;
    // request 只是发起异步 RPC，不会阻塞 UI。结果稍后统一进入
    // handleProviderResponse()，类似前端 $.ajax(...).done(...) 的集中式写法。
    provider_.request(
        {{"id", QStringLiteral("save-%1-%2").arg(currentQuestionIndex_).arg(choice)},
         {"method", "attempt.saveAnswers"},
         {"params", QJsonObject{{"attemptId", attemptId_}, {"answers", payload}}}},
        &error);
    saveDraft();
    // 留出极短的选中反馈，再自动进入下一题。若用户已经手动切题，index 校验
    // 会阻止旧定时器把新页面再次向前推进。
    const int answeredIndex = currentQuestionIndex_;
    if (answeredIndex + 1 < questions_.size()) {
        QTimer::singleShot(120, this, [this, answeredIndex] {
            if (currentQuestionIndex_ == answeredIndex)
                showQuestion(answeredIndex + 1);
        });
    } else {
        // 最后一题没有“下一题”可跳，短暂停留显示选中反馈后直接询问是否交卷。
        QTimer::singleShot(120, this, [this, answeredIndex] {
            if (currentQuestionIndex_ == answeredIndex)
                submitAttempt();
        });
    }
}

QJsonArray MainWindow::answerPayload() const {
    QJsonArray payload;
    for (qsizetype index = 0; index < questions_.size(); ++index) {
        const int selected = answers_.value(index, -1);
        QJsonValue choice = selected >= 0 ? QJsonValue(QString::number(selected))
                                          : QJsonValue(QJsonValue::Null);
        payload.append(QJsonObject{
            {"questionIndex", index},
            {"questionId", questions_.at(index).toObject().value("id").toString()},
            {"time", 0}, {"flag", 0},
            {"answer", QJsonObject{{"type", 201}, {"choice", choice}}}});
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
                             submitConfirmationBubble_->height() - 7);
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
                                            material.value("contentHtml").toString());
    }
    QString optionsHtml;
    for (const auto& optionValue : solution.value("options").toArray()) {
        const auto option = optionValue.toObject();
        optionsHtml += QStringLiteral("<p><b>%1.</b> %2</p>")
            .arg(option.value("label").toString().toHtmlEscaped(),
                 plainText(option.value("contentHtml").toString()).toHtmlEscaped());
    }
    solutionQuestionLabel_->setText(
        QStringLiteral("<div style=\"color:#c7ccd2\">%1%2</div>")
            .arg(solution.value("contentHtml").toString(), optionsHtml));
    const int correct = solution.value("correctChoice").toInt(-1);
    const int selected = answers_.value(currentSolutionIndex_, -1);
    solutionAnswerLabel_->setText(QStringLiteral("你的答案：%1　正确答案：%2　%3")
        .arg(choiceLabel(selected), choiceLabel(correct),
             selected == correct ? QStringLiteral("✓ 正确") : QStringLiteral("✗ 错误")));
    solutionAnswerLabel_->setProperty("correct", selected == correct);
    solutionAnswerLabel_->style()->unpolish(solutionAnswerLabel_);
    solutionAnswerLabel_->style()->polish(solutionAnswerLabel_);
    solutionExplanationLabel_->setText(
        QStringLiteral("<div style=\"color:#aebbb5\"><p><b>解析</b></p>%1</div>")
            .arg(solution.value("solutionHtml").toString()));
    solutionProgressLabel_->setText(QStringLiteral("第 %1 / %2 题")
        .arg(currentSolutionIndex_ + 1).arg(solutions_.size()));
    previousSolutionButton_->setEnabled(currentSolutionIndex_ > 0);
    nextSolutionButton_->setEnabled(currentSolutionIndex_ + 1 < solutions_.size());
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
        for (int y = 0; y < code.getSize(); ++y)
            for (int x = 0; x < code.getSize(); ++x)
                if (code.getModule(x, y))
                    for (int dy = 0; dy < scale; ++dy)
                        for (int dx = 0; dx < scale; ++dx)
                            image.setPixelColor((x + border) * scale + dx,
                                                (y + border) * scale + dy, Qt::black);
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
    // id 相当于前端 Promise/后端 traceId；同一时刻可并行等待报告和题目解析。
    const QString id = response.value("id").toString();
    diagnostic::event(QStringLiteral("ui"), QStringLiteral("provider-response-route"),
        {{QStringLiteral("id"), id},
         {QStringLiteral("error"), response.contains(QStringLiteral("error"))}});
    if (response.contains("error")) {
        const QString message = userFacingError(
            response.value("error").toObject().value("message").toString());
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
    const QJsonObject result = response.value("result").toObject();
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
        submitButton_->setEnabled(true);
        showQuestion(0);
        pages_->setCurrentWidget(practicePage_);
        saveDraft();
    } else if (id == QStringLiteral("final-save")) {
        sendSubmit();
    } else if (id == QStringLiteral("attempt-submit")) {
        draftStore_.clear(providerId_);
        requestResults();
    } else if (id == QStringLiteral("attempt-report")) {
        resultSummaryLabel_->setText(QStringLiteral("%1 / %2 题正确")
            .arg(result.value("correctCount").toInt())
            .arg(result.value("questionCount").toInt()));
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
    const int viewportHeight = qMax(120, event->size().height() - fixedHeight);
    if (questionScroll_->height() != viewportHeight) {
        lockedPracticeViewportHeight_ = viewportHeight;
        questionScroll_->setFixedHeight(viewportHeight);
    }
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
    const auto answer = QMessageBox::question(
        this, QStringLiteral("确认导入题库"),
        QStringLiteral("%1\n版本：%2\n权限：%3\n\n"
                       "导入后即可在小窗刷题中使用。是否继续？")
            .arg(package.name, package.version, permission));
    if (answer != QMessageBox::Yes) return;

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

void MainWindow::loadProvider(const QString& path) {
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
    materialsById_.clear();
    practiceMaterialCard_->hideMaterial();
    solutionMaterialCard_->hideMaterial();
    answers_.clear();
    QString error;
    if (!provider_.load(path, &error)) {
        diagnostic::event(QStringLiteral("ui"), QStringLiteral("load-provider-failed"),
            {{QStringLiteral("error"), error}});
        titleLabel_->setText(QStringLiteral("题库加载失败"));
        detailLabel_->setText(error);
        return;
    }
    const auto descriptor = provider_.descriptor();
    providerId_ = descriptor.value("id").toString();
    diagnostic::event(QStringLiteral("ui"), QStringLiteral("load-provider-success"),
        {{QStringLiteral("id"), providerId_},
         {QStringLiteral("version"), descriptor.value("version").toString()}});
    currentProviderPath_ = QFileInfo(path).absoluteFilePath();
    QSettings().setValue(QStringLiteral("provider/lastLibraryPath"),
                         QFileInfo(path).absoluteFilePath());
    draftRestoreChecked_ = false;
    sourceLabel_->setText(descriptor.value("name").toString(QStringLiteral("题库")));
    titleLabel_->setText(QStringLiteral("正在打开题库…"));
    detailLabel_->setText(QStringLiteral("请稍候"));
    sendInitialize();
}

bool MainWindow::loadLastProvider() {
    QString path = QSettings()
        .value(QStringLiteral("provider/lastLibraryPath")).toString();
    if (path.isEmpty()) {
        QSettings legacySettings(QStringLiteral("QuizPane Project"),
                                 QStringLiteral("QuizPane"));
        path = legacySettings.value(QStringLiteral("provider/lastLibraryPath"))
                   .toString();
    }
    if (path.isEmpty() || !QFileInfo(path).isFile()) {
        const auto installed = installer_.listInstalled();
        if (!installed.isEmpty()) path = installed.first().entryPath;
    }
    if (path.isEmpty() || !QFileInfo(path).isFile()) return false;
    loadProvider(path);
    return provider_.isLoaded();
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

// ===== 用户菜单与独立题库制作器 =====

void MainWindow::showUiSizeMenu() {
    QMenu menu(this);
    QAction* small = menu.addAction(QStringLiteral("小"));
    QAction* medium = menu.addAction(QStringLiteral("中"));
    QAction* large = menu.addAction(QStringLiteral("大"));
    small->setCheckable(true);
    medium->setCheckable(true);
    large->setCheckable(true);
    small->setChecked(uiSize_ == UiSize::Small);
    medium->setChecked(uiSize_ == UiSize::Medium);
    large->setChecked(uiSize_ == UiSize::Large);
    QAction* selected = menu.exec(
        sizeButton_->mapToGlobal(QPoint(0, sizeButton_->height())));
    if (selected == small) applyUiSize(UiSize::Small);
    else if (selected == medium) applyUiSize(UiSize::Medium);
    else if (selected == large) applyUiSize(UiSize::Large);
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
    if (installed.isEmpty()) {
        QAction* emptySwitch = switchMenu->addAction(QStringLiteral("暂无已添加题库"));
        QAction* emptyDelete = deleteMenu->addAction(QStringLiteral("暂无可删除题库"));
        emptySwitch->setEnabled(false);
        emptyDelete->setEnabled(false);
    } else {
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
    QMessageBox::information(this, QStringLiteral("尚未安装题库制作器"),
        QStringLiteral("题库制作器是独立工具，请安装对应版本后重试。"));
}

void MainWindow::configureBossKey() {
    const auto requestedValue = ui::askBossKey(this, bossKey_);
    if (!requestedValue) return;
    const QKeySequence requested = *requestedValue;
    const QKeySequence previous = bossKey_;
    QString error;
    if (!globalHotkey_->registerBossKey(requested, &error)) {
        globalHotkey_->registerBossKey(previous);
        QMessageBox::warning(this, QStringLiteral("无法设置老板键"), error);
        return;
    }
    bossKey_ = requested;
    QSettings().setValue(QStringLiteral("bossKey/sequence"),
                         bossKey_.toString(QKeySequence::PortableText));
    showHideAction_->setShortcut(bossKey_);
    if (trayIcon_)
        trayIcon_->setToolTip(QStringLiteral("小窗刷题 · %1")
            .arg(bossKey_.toString(QKeySequence::NativeText)));
}

void MainWindow::showAboutDialog() {
    ui::showAbout(this);
}

void MainWindow::switchProvider(const InstalledProviderInfo& provider) {
    if (provider.id == providerId_) return;
    loadProvider(provider.entryPath);
}

void MainWindow::deleteProvider(const InstalledProviderInfo& provider) {
    const bool current = provider.id == providerId_;
    const QString message = current
        ? QStringLiteral("正在使用“%1”。删除后将返回题库导入页。\n\n"
                         "登录状态会保留，重新添加后仍可恢复。确定删除吗？")
              .arg(provider.name)
        : QStringLiteral("确定删除“%1”吗？\n\n"
                         "登录状态会保留，重新添加后仍可恢复。")
              .arg(provider.name);
    if (QMessageBox::question(this, QStringLiteral("删除题库"), message) !=
        QMessageBox::Yes) return;

    if (current) {
        provider_.unload();
        providerId_.clear();
        currentProviderPath_.clear();
        QSettings().remove(QStringLiteral("provider/lastLibraryPath"));
    }
    QString error;
    if (!installer_.removeInstalled(provider.id, &error)) {
        QSettings settings;
        QStringList pending = settings.value(
            QStringLiteral("providers/pendingDelete")).toStringList();
        if (!pending.contains(provider.id)) pending.append(provider.id);
        settings.setValue(QStringLiteral("providers/pendingDelete"), pending);
        QMessageBox::information(this, QStringLiteral("将在重启后删除"), error);
    }
    if (current) showProviderOnboarding();
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
    QSettings settings;
    const QStringList pending = settings.value(
        QStringLiteral("providers/pendingDelete")).toStringList();
    QStringList remaining;
    for (const QString& id : pending) {
        QString error;
        if (!installer_.removeInstalled(id, &error)) remaining.append(id);
    }
    settings.setValue(QStringLiteral("providers/pendingDelete"), remaining);
}

// ===== 窗口尺寸、置顶和视觉样式 =====

void MainWindow::applyUiSize(UiSize size) {
    // Qt 的布局系统类似浏览器 layout：先设置控件约束，再由 layout 计算最终几何。
    // 答题页保留用户手动调整的高度，登录页则按内容 sizeHint 自动收紧。
    uiSize_ = size;
    QString property;
    QSize windowSize;
    constexpr int margin = 14;
    constexpr int spacing = 7;
    constexpr int iconPixels = 15;
    constexpr int iconButtonPixels = 26;
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
    headerBar_->setFixedHeight(28);
    practiceControlBar_->setFixedHeight(30);
    solutionControlBar_->setFixedHeight(30);
    const QSize iconSize(iconPixels, iconPixels);
    for (auto* button : card_->findChildren<QPushButton*>()) {
        const QString name = button->objectName();
        if (name != QStringLiteral("navIconButton") &&
            name != QStringLiteral("headerIconButton") &&
            name != QStringLiteral("pinButton") &&
            name != QStringLiteral("closeButton")) continue;
        button->setIconSize(iconSize);
        button->setFixedSize(iconButtonPixels, iconButtonPixels);
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
    QSettings().setValue(QStringLiteral("ui/size"), property);
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
    questionScroll_->setFixedHeight(lockedPracticeViewportHeight_);
    questionScroll_->verticalScrollBar()->setSingleStep(24);

    practicePage_->layout()->invalidate();
    practicePage_->layout()->activate();
    const int pageHeight = practicePage_->layout()->sizeHint().height();
    const auto* root = qobject_cast<QVBoxLayout*>(card_->layout());
    const QMargins margins = root->contentsMargins();
    const int totalHeight = margins.top() + margins.bottom() +
                            headerBar_->height() + resizeHandle_->height() +
                            root->spacing() * 2 + pageHeight;
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
    QSettings().setValue(QStringLiteral("window/pinned"), pinned_);
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

void MainWindow::applyCardStyle() {
    setStyleSheet(R"(
        QWidget#card {
            background: rgba(12, 14, 18, 96);
            border: none;
            border-radius: 0;
            color: #c8cdd3;
        }
        QWidget#card[uiSize="small"] { font-size: 11px; }
        QWidget#card[uiSize="medium"] { font-size: 12px; }
        QWidget#card[uiSize="large"] { font-size: 13px; }
        QWidget#card QLabel { color: #c8cdd3; }
        QWidget#card QScrollArea {
            background: transparent;
            border: none;
        }
        QWidget#card QScrollArea > QWidget > QWidget { background: transparent; }
        QWidget#card QScrollBar:vertical {
            background: transparent;
            width: 7px;
            margin: 3px 2px 3px 2px;
        }
        QWidget#card QScrollBar::handle:vertical {
            background: rgba(220,225,232,62);
            border: none;
            border-radius: 1px;
            min-height: 26px;
        }
        QWidget#card QScrollBar::handle:vertical:hover {
            background: rgba(225,230,236,100);
        }
        QWidget#card QScrollBar::handle:vertical:pressed {
            background: rgba(230,234,240,132);
        }
        QWidget#card QScrollBar::add-line:vertical,
        QWidget#card QScrollBar::sub-line:vertical {
            background: transparent;
            border: none;
            width: 0;
            height: 0;
        }
        QWidget#card QScrollBar::add-page:vertical,
        QWidget#card QScrollBar::sub-page:vertical { background: transparent; }
        QWidget#card QScrollBar::up-arrow:vertical,
        QWidget#card QScrollBar::down-arrow:vertical { width: 0; height: 0; }
        QLabel#sourceTitle { color: #aeb4bb; font-weight: 500; }
        QLabel#title, QLabel#pageTitle { color: #c6cbd1; font-weight: 550; }
        QWidget#card[uiSize="small"] QLabel#title,
        QWidget#card[uiSize="small"] QLabel#pageTitle { font-size: 13px; }
        QWidget#card[uiSize="medium"] QLabel#title,
        QWidget#card[uiSize="medium"] QLabel#pageTitle { font-size: 15px; }
        QWidget#card[uiSize="large"] QLabel#title,
        QWidget#card[uiSize="large"] QLabel#pageTitle { font-size: 17px; }
        QLabel#detail { color: #9ca3ab; }
        QLabel#questionText {
            color: #d5d1c5;
            background: rgba(18, 20, 24, 44);
            border: none;
            border-radius: 5px;
            padding: 7px;
            font-weight: 550;
        }
        QWidget#card[uiSize="small"] QLabel#questionText { font-size: 12px; }
        QWidget#card[uiSize="medium"] QLabel#questionText { font-size: 13px; }
        QWidget#card[uiSize="large"] QLabel#questionText { font-size: 15px; }
        QLabel#solutionQuestion { color: #c7ccd2; }
        QLabel#resultAnswer { color: #c2b7a3; }
        QWidget#materialCard {
            background: rgba(24, 27, 32, 60);
            border: none;
            border-radius: 5px;
        }
        QLabel#materialCardTitle { color: #b7bec7; font-weight: 600; }
        QWidget#card[uiSize="small"] QLabel#materialCardTitle { font-size: 11px; }
        QWidget#card[uiSize="medium"] QLabel#materialCardTitle { font-size: 12px; }
        QWidget#card[uiSize="large"] QLabel#materialCardTitle { font-size: 13px; }
        QPushButton#materialCardToggle { background: transparent; padding: 0; }
        QLabel#materialCardBody { color: #a9b0b8; }
        QWidget#card[uiSize="small"] QLabel#materialCardBody { font-size: 11px; }
        QWidget#card[uiSize="medium"] QLabel#materialCardBody { font-size: 12px; }
        QWidget#card[uiSize="large"] QLabel#materialCardBody { font-size: 13px; }
        QLabel#solutionText {
            color: #aebbb5;
            background: rgba(28, 36, 33, 48);
            border: none;
            border-radius: 5px;
            padding: 7px;
        }
        QWidget#catalogRow {
            background: rgba(20, 23, 28, 58);
            border: none;
            border-radius: 7px;
        }
        QPushButton {
            background: rgba(255,255,255,10);
            color: #b9c0c8;
            border: none;
            border-radius: 6px;
            padding: 5px 8px;
            font-weight: 500;
        }
        QPushButton#smallButton {
            background: rgba(255,255,255,8);
            padding: 4px 6px;
            min-width: 30px;
            color: #aeb5bd;
        }
        QPushButton#navIconButton,
        QPushButton#headerIconButton,
        QPushButton#pinButton,
        QPushButton#closeButton { background: transparent; padding: 0; }
        QPushButton:disabled { background: transparent; color: rgba(170,176,184,70); }
        QRadioButton#answerOption {
            color: #c8cdd3;
            background: rgba(18, 21, 26, 52);
            border: none;
            border-radius: 6px;
            padding: 8px;
        }
        QRadioButton#answerOption::indicator { width: 0; height: 0; }
        QRadioButton#answerOption:checked {
            background: rgba(180, 188, 198, 38);
            color: #e0e3e7;
        }
        QLabel[correct="true"] { color: #9fb6a7; font-weight: 550; }
        QLabel[correct="false"] { color: #b89d9c; font-weight: 550; }
        QPushButton#pinButton:checked {
            background: rgba(255,255,255,18);
        }
        QFrame#submitConfirmationBubble {
            background: rgba(25, 28, 33, 236);
            border: none;
            border-radius: 9px;
        }
        QLabel#submitConfirmationText {
            color: #d0d4d9;
            background: transparent;
        }
        QPushButton#confirmationSecondary {
            background: transparent;
            color: #9299a2;
            padding: 4px 7px;
        }
        QPushButton#confirmationPrimary {
            background: rgba(255,255,255,16);
            color: #cbd0d6;
            padding: 4px 7px;
        }
        QPushButton:hover { background: rgba(255,255,255,18); }
        QPushButton:pressed { background: rgba(255,255,255,28); }
    )");
}

}  // namespace quizpane
