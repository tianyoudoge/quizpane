#pragma once

#include <QHash>
#include <QJsonArray>
#include <QMainWindow>
#include <QElapsedTimer>
#include <QKeySequence>
#include <QVector>
#include <QSet>

#include "../app_services.hpp"

class QLabel;
class QLayout;
class QAction;
class QButtonGroup;
class QMenu;
class QFrame;
class QShowEvent;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QTimer;
class QVBoxLayout;
class QSystemTrayIcon;

namespace quizpane::ui {
class MaterialCard;
class QuestionNavigator;
}

namespace quizpane {

class GlobalHotkey;

// MainWindow 负责装配页面并把题库请求交给应用服务。成员中的控件指针均为
// Qt 父子对象树持有的非拥有引用。
class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
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
    void startAttempt(const QString& categoryId, const QString& title, int count,
                      bool includePreviouslyAnswered = false);
    void requestQuestions();
    void updateMaterialsCache(const QJsonArray& materials);
    void showQuestion(int index);
    void chooseAnswer(int choice, bool checked = true);
    void toggleQuestionNavigator();
    void refreshQuestionNavigator();
    QJsonArray answerPayload() const;
    void submitAttempt();
    void confirmSubmitAttempt();
    void hideSubmitConfirmation();
    void positionSubmitConfirmation();
    void sendSubmit();
    void requestResults();
    void showSolution(int index);
    void exportAttemptResults();
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
    AppServices services_;
    ProviderLoader& provider_ = services_.provider();
    ProviderInstaller& installer_ = services_.installer();
    DraftStore& draftStore_ = services_.drafts();

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
    QButtonGroup* optionButtonGroup_ = nullptr;
    ui::MaterialCard* practiceMaterialCard_ = nullptr;
    ui::MaterialCard* solutionMaterialCard_ = nullptr;
    QVBoxLayout* solutionContentLayout_ = nullptr;
    QWidget* practiceControlBar_ = nullptr;
    QPushButton* previousQuestionButton_ = nullptr;
    QPushButton* nextQuestionButton_ = nullptr;
    QPushButton* questionListButton_ = nullptr;
    QPushButton* submitButton_ = nullptr;
    ui::QuestionNavigator* questionNavigator_ = nullptr;
    QFrame* submitConfirmationBubble_ = nullptr;
    QLabel* submitConfirmationLabel_ = nullptr;
    QLabel* resultSummaryLabel_ = nullptr;
    QLabel* solutionProgressLabel_ = nullptr;
    QLabel* solutionQuestionLabel_ = nullptr;
    QLabel* solutionAnswerLabel_ = nullptr;
    QLabel* selectedAnswerLabel_ = nullptr;
    QLabel* correctAnswerLabel_ = nullptr;
    QLabel* answerStatusLabel_ = nullptr;
    QLabel* solutionExplanationLabel_ = nullptr;
    QPushButton* previousSolutionButton_ = nullptr;
    QPushButton* nextSolutionButton_ = nullptr;
    QPushButton* exportResultsButton_ = nullptr;
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
    bool attemptHasAnswerKey_ = true;
    // materialId -> material（title/contentHtml）。attempt.questions 和
    // attempt.solutions 各自返回去重后的材料数组，这里合并成一份缓存供两个
    // 页面共用，避免每次切题都重新在数组里线性查找。
    QHash<QString, QJsonObject> materialsById_;
    QVector<int> answers_;
    // 单选仍用 answers_ 保持草稿兼容；多选保存完整集合，判分和 RPC 不会丢项。
    QVector<QSet<int>> multiAnswers_;
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
