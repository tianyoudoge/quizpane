#pragma once

#include <QHash>
#include <QJsonArray>
#include <QMainWindow>
#include <QElapsedTimer>
#include <QKeySequence>
#include <QVector>

#include "quizpane/provider_installer.hpp"
#include "quizpane/provider_loader.hpp"
#include "quizpane/draft_store.hpp"

class QLabel;
class QLayout;
class QAction;
class QMenu;
class QFrame;
class QShowEvent;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QTimer;
class QVBoxLayout;
class QSystemTrayIcon;

namespace quizpane::ui { class MaterialCard; }

namespace quizpane {

class GlobalHotkey;

// MainWindow 相当于传统 jQuery 单页应用中的页面控制器：创建控件、绑定事件，
// 再把题库请求交给 ProviderLoader。Qt 父子对象树会自动释放 QObject 子对象，
// 所以成员中的裸指针是“非拥有引用”，不是需要手工 delete 的 Java 式资源。
class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    void loadProvider(const QString& path);
    void installProviderPackage(const QString& path);
    bool loadLastProvider();

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    // ActionMode 是登录/引导页主按钮的有限状态机，避免用多组 bool 拼状态。
    enum class ActionMode { InstallProvider, ConnectAccount, CancelLogin, OpenCatalog };
    enum class UiSize { Small, Medium, Large };

    void chooseProviderPackage();
    void openBankStudio();
    void runPrimaryAction();
    void beginLogin();
    void pollLogin();
    void requestCatalog();
    void setQrContent(const QString& content);
    void setActionMode(ActionMode mode, const QString& text);
    void handleProviderResponse(const QJsonObject& response);
    void populateCatalog(const QJsonArray& nodes);
    void startAttempt(const QString& categoryId, const QString& title, int count);
    void requestQuestions();
    void updateMaterialsCache(const QJsonArray& materials);
    void showQuestion(int index);
    void chooseAnswer(int choice);
    QJsonArray answerPayload() const;
    void submitAttempt();
    void confirmSubmitAttempt();
    void hideSubmitConfirmation();
    void positionSubmitConfirmation();
    void sendSubmit();
    void requestResults();
    void showSolution(int index);
    void clearLayout(QLayout* layout);
    void setPinned(bool pinned);
    void showUiSizeMenu();
    void applyUiSize(UiSize size);
    void lockCompactPracticeHeight();
    int answerViewportMaximumHeight() const;
    void initializeDesktopShell();
    void toggleWindowVisibility();
    void returnToCatalog();
    void saveDraft();
    bool maybeRestoreDraft();
    void sendInitialize();
    void applyCardStyle();
    void adjustWindowForCurrentPage();
    void showMainMenu();
    void configureBossKey();
    void showAboutDialog();
    void switchProvider(const InstalledProviderInfo& provider);
    void deleteProvider(const InstalledProviderInfo& provider);
    void exportDeclarativeProvider(const InstalledProviderInfo& provider);
    void showProviderOnboarding();
    void processPendingProviderDeletions();

    // ---- 应用服务：值成员由 MainWindow 直接拥有，析构顺序与声明顺序相反 ----
    ProviderLoader provider_;
    ProviderInstaller installer_;
    DraftStore draftStore_;

    // ---- 桌面外壳：全局热键、托盘和菜单 ----
    GlobalHotkey* globalHotkey_ = nullptr;
    QSystemTrayIcon* trayIcon_ = nullptr;
    QMenu* trayMenu_ = nullptr;
    QAction* showHideAction_ = nullptr;
    QAction* pinAction_ = nullptr;
    QPoint dragOffset_;

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
    QLabel* titleLabel_ = nullptr;
    QLabel* detailLabel_ = nullptr;
    QLabel* qrLabel_ = nullptr;
    QPushButton* actionButton_ = nullptr;
    QPushButton* createBankButton_ = nullptr;
    QTimer* loginPollTimer_ = nullptr;
    QVBoxLayout* catalogListLayout_ = nullptr;
    QLabel* practiceTitleLabel_ = nullptr;
    QLabel* practiceProgressLabel_ = nullptr;
    QLabel* questionLabel_ = nullptr;
    QScrollArea* questionScroll_ = nullptr;
    QWidget* questionContent_ = nullptr;
    QVBoxLayout* questionContentLayout_ = nullptr;
    QVBoxLayout* optionsLayout_ = nullptr;
    ui::MaterialCard* practiceMaterialCard_ = nullptr;
    ui::MaterialCard* solutionMaterialCard_ = nullptr;
    QVBoxLayout* solutionContentLayout_ = nullptr;
    QWidget* practiceControlBar_ = nullptr;
    QPushButton* previousQuestionButton_ = nullptr;
    QPushButton* nextQuestionButton_ = nullptr;
    QPushButton* submitButton_ = nullptr;
    QFrame* submitConfirmationBubble_ = nullptr;
    QLabel* submitConfirmationLabel_ = nullptr;
    QLabel* resultSummaryLabel_ = nullptr;
    QLabel* solutionProgressLabel_ = nullptr;
    QLabel* solutionQuestionLabel_ = nullptr;
    QLabel* solutionAnswerLabel_ = nullptr;
    QLabel* solutionExplanationLabel_ = nullptr;
    QPushButton* previousSolutionButton_ = nullptr;
    QPushButton* nextSolutionButton_ = nullptr;
    QWidget* solutionControlBar_ = nullptr;
    QWidget* resizeHandle_ = nullptr;
    // ---- 当前页面会话状态：切换题库时会重置，安全凭据不在这里 ----
    ActionMode actionMode_ = ActionMode::InstallProvider;
    QString loginSessionId_;
    QString providerId_;
    QString attemptId_;
    QString attemptTitle_;
    QJsonArray questions_;
    QJsonArray solutions_;
    // materialId -> material（title/contentHtml）。attempt.questions 和
    // attempt.solutions 各自返回去重后的材料数组，这里合并成一份缓存供两个
    // 页面共用，避免每次切题都重新在数组里线性查找。
    QHash<QString, QJsonObject> materialsById_;
    QVector<int> answers_;
    int currentQuestionIndex_ = 0;
    int currentSolutionIndex_ = 0;
    // ---- 用户偏好与窗口几何 ----
    bool pinned_ = true;
    UiSize uiSize_ = UiSize::Medium;
    int lockedPracticeViewportHeight_ = 0;
    bool draftRestoreChecked_ = false;
    QElapsedTimer visibilityToggleDebounce_;
    QSize standardWindowSize_{380, 560};
    QString qrContent_;
    QKeySequence bossKey_{QStringLiteral("Ctrl+Shift+H")};
    QString currentProviderPath_;
};

}  // namespace quizpane
