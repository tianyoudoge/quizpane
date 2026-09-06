#pragma once

#include <QMainWindow>
#include <QElapsedTimer>
#include <QKeySequence>

#include "../app_services.hpp"
#include "attempt_session.hpp"
#include "catalog_page_controller.hpp"
#include "practice_page_controller.hpp"
#include "quizpane/provider_response_router.hpp"
#include "solution_page_controller.hpp"
#include "ui_size.hpp"

class QLabel;
class QLayout;
class QAction;
class QMenu;
class QFrame;
class QFile;
class QNetworkAccessManager;
class QNetworkReply;
class QProgressDialog;
class QShowEvent;
class QPushButton;
class QStackedWidget;
class QTimer;
class QSystemTrayIcon;

namespace quizpane {

class GlobalHotkey;
#if defined(QUIZPANE_HAVE_WEBSOCKETS)
namespace browser {
class BrowserBridge;
struct BrowserStatus;
}
#endif

// MainWindow 负责装配页面并把题库请求交给应用服务。做题页/解析页/目录页的
// 控件、状态和交互逻辑已分别拆分到 PracticePageController/
// SolutionPageController/CatalogPageController；MainWindow 只保留跨页面服务
// 引用、桌面外壳（托盘、热键、更新下载）、顶层页面装配/切换和登录页逻辑。
// 成员中的控件指针均为 Qt 父子对象树持有的非拥有引用。
class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;
    bool loadProvider(const QString& path);
    void installProviderPackage(const QString& path);
    bool loadLastProvider();

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    // ActionMode 是登录/引导页主按钮的有限状态机，避免用多组 bool 拼状态。
    enum class ActionMode { InstallProvider, ConnectAccount, CancelLogin, OpenCatalog };

    void chooseProviderPackage();
    void openBankStudio();
    void runPrimaryAction();
    void beginLogin();
    void pollLogin();
    void requestCatalog();
    void setQrContent(const QString& content);
    void setActionMode(ActionMode mode, const QString& text);
    void handleProviderResponse(const QJsonObject& response);
    void handleProviderError(const ProviderResponseEnvelope& envelope);
    void handleInitializeResponse(const QJsonObject& result);
    void handleAuthBeginResponse(const QJsonObject& result);
    void handleAuthPollResponse(const QJsonObject& result);
    void handleCatalogResponse(const QJsonObject& result);
    void handleAttemptCreateResponse(const QJsonObject& result);
    void handleQuestionsResponse(const QJsonObject& result);
    void handleReportResponse(const QJsonObject& result);
    void handleSolutionsResponse(const QJsonObject& result);
    void setPinned(bool pinned);
    void showUiSizeMenu();
    void applyUiSize(UiSize size);
    void showBackgroundVisibilityDialog();
    void applyBackgroundVisibility(int value);
    void initializeDesktopShell();
    void toggleWindowVisibility();
    void returnToCatalog();
    void sendInitialize();
    void applyCardStyle();
    void adjustWindowForCurrentPage();
    void showMainMenu();
    void configureBossKey();
    void showAboutDialog();
    void checkForUpdates();
    void downloadAndInstallUpdate(const QString& tag, const QString& asset,
                                  const QString& expectedSha256);
    void completeUpdateDownload();
    bool startDownloadedUpdate();
    void resetUpdateDownload();
    void switchProvider(const InstalledProviderInfo& provider);
    void deleteProvider(const InstalledProviderInfo& provider);
    void exportDeclarativeProvider(const InstalledProviderInfo& provider);
    void showProviderOnboarding();
    void processPendingProviderDeletions();
#if defined(QUIZPANE_HAVE_WEBSOCKETS)
    void updateCourseCompanion(const browser::BrowserStatus& status);
    void showCourseCompanion();
    void continueFromCourseCompanion();
    void fitCourseCompanionWindow();
#endif

    // ---- 应用服务：值成员由 MainWindow 直接拥有，析构顺序与声明顺序相反 ----
    AppServices services_;
    ProviderLoader& provider_ = services_.provider();
    ProviderInstaller& installer_ = services_.installer();
    DraftStore& draftStore_ = services_.drafts();

    // ---- 桌面外壳：全局热键、托盘和菜单 ----
    GlobalHotkey* globalHotkey_ = nullptr;
#if defined(QUIZPANE_HAVE_WEBSOCKETS)
    browser::BrowserBridge* browserBridge_ = nullptr;
#endif
    QSystemTrayIcon* trayIcon_ = nullptr;
    QMenu* trayMenu_ = nullptr;
    QAction* showHideAction_ = nullptr;
    QAction* pinAction_ = nullptr;
    QPoint dragOffset_;

    // Release 元数据和安装包只会在用户手动选择“检查更新”后请求。下载文件放在
    // 临时目录；更新脚本在本进程退出后才替换程序目录，因此不会碰用户数据目录。
    QNetworkAccessManager* updateNetworkManager_ = nullptr;
    QNetworkReply* updateReply_ = nullptr;
    QFile* updateDownloadFile_ = nullptr;
    QProgressDialog* updateProgress_ = nullptr;
    QString updateTag_;
    QString updateAsset_;
    QString updateExpectedSha256_;
    QString updateDownloadPath_;

    // ---- 控件引用：实际所有权在 Qt parent/child 对象树中 ----
    QWidget* card_ = nullptr;
    QWidget* headerBar_ = nullptr;
    QLabel* sourceLabel_ = nullptr;
    QPushButton* pinButton_ = nullptr;
    QPushButton* sizeButton_ = nullptr;
    QPushButton* menuButton_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QWidget* loginPage_ = nullptr;
    QWidget* catalogPage_ = nullptr;
    QWidget* practicePage_ = nullptr;
    QWidget* solutionPage_ = nullptr;
#if defined(QUIZPANE_HAVE_WEBSOCKETS)
    QWidget* courseCompanionPage_ = nullptr;
    QLabel* courseCompanionTitleLabel_ = nullptr;
    QLabel* courseCompanionStatusLabel_ = nullptr;
    QLabel* courseCompanionProgressLabel_ = nullptr;
    QLabel* courseCompanionNoticeLabel_ = nullptr;
    QPushButton* courseTogglePlaybackButton_ = nullptr;
    QPushButton* coursePermissionButton_ = nullptr;
    QFrame* practiceVideoStatusBar_ = nullptr;
    QLabel* practiceVideoStatusLabel_ = nullptr;
    QPushButton* practiceVideoDetailsButton_ = nullptr;
    QWidget* pageBeforeCourseCompanion_ = nullptr;
    bool browserCourseWasBound_ = false;
#endif
    QLabel* titleLabel_ = nullptr;
    QLabel* detailLabel_ = nullptr;
    QLabel* qrLabel_ = nullptr;
    QPushButton* actionButton_ = nullptr;
    QPushButton* createBankButton_ = nullptr;
    QTimer* loginPollTimer_ = nullptr;
    QWidget* resizeHandle_ = nullptr;

    // ---- 三个页面控制器：各自持有对应页面的控件与状态 ----
    AttemptSession session_;
    CatalogPageController catalogController_;
    PracticePageController practiceController_;
    SolutionPageController solutionController_;

    // ---- 当前页面会话状态：切换题库时会重置，安全凭据不在这里 ----
    ActionMode actionMode_ = ActionMode::InstallProvider;
    QString loginSessionId_;
    QString providerId_;
    // ---- 用户偏好与窗口几何 ----
    bool pinned_ = true;
    UiSize uiSize_ = UiSize::Medium;
    int backgroundVisibility_ = 100;
    bool draftRestoreChecked_ = false;
    QElapsedTimer visibilityToggleDebounce_;
    QSize standardWindowSize_{380, 560};
    QString qrContent_;
    QKeySequence bossKey_{QStringLiteral("Ctrl+H")};
    QString currentProviderPath_;
};

}  // namespace quizpane
