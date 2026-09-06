#pragma once

#include "attempt_session.hpp"
#include "ui_size.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

class QLabel;
class QScrollArea;
class QWidget;
class QVBoxLayout;
class QButtonGroup;
class QPushButton;
class QStackedWidget;
class QFrame;

namespace quizpane {
class ProviderLoader;
class DraftStore;
namespace ui {
class MaterialCard;
class QuestionNavigator;
}

// 做题页（第三个顶层页面）的控件、状态和交互逻辑。
// 非 QObject 类，由 MainWindow 作为值成员持有；不持有 MainWindow*，
// 跨页面共享状态（当前作答会话）通过 AttemptSession& 引用访问。
class PracticePageController {
public:
    PracticePageController() = default;

    // 构造后、buildInto() 前必须先调用一次。practicePage/pages/catalogPage/
    // headerBar/card/resizeHandle 均由 MainWindow 装配、传入非拥有指针；
    // session/uiSize/providerId/draftRestoreChecked 是跨控制器共享状态，
    // 物理上仍属于 MainWindow，这里只持有引用，不拷贝。
    void init(QWidget* practicePage, QStackedWidget* pages, QWidget* catalogPage,
              QWidget* headerBar, QWidget* card, QWidget* resizeHandle,
              ProviderLoader& provider, DraftStore& draftStore,
              AttemptSession& session, UiSize& uiSize,
              QString& providerId, bool& draftRestoreChecked);

    // 构建做题页内部控件并接入 practicePage_；调用前必须先调用 init()。
    void buildInto();

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
    void saveDraft();
    bool maybeRestoreDraft();
    void lockCompactPracticeHeight();
    int answerViewportMaximumHeight() const;

    // 供 MainWindow 在 resizeEvent/keyPressEvent/handleQuestionsResponse 等
    // 路由与顶层事件里读取的状态与子控件。
    int currentQuestionIndex() const { return currentQuestionIndex_; }
    int questionCount() const { return static_cast<int>(session_->questions.size()); }
    int currentOptionCount() const;
    // pages_->currentWidget()/practicePage_ 比较、几何计算均在内部完成；
    // MainWindow::resizeEvent 只需转发事件本身。
    void handleResize();
    void resetForNewQuestions();
    void setSubmitEnabled(bool enabled);
    QWidget* controlBar() const;
    ui::QuestionNavigator* navigator() const { return questionNavigator_; }
    QFrame* submitConfirmationBubble() const { return submitConfirmationBubble_; }
    bool isCurrentPage() const;

private:
    QWidget* practicePage_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QWidget* catalogPage_ = nullptr;
    QWidget* headerBar_ = nullptr;
    QWidget* card_ = nullptr;
    QWidget* resizeHandle_ = nullptr;
    ProviderLoader* provider_ = nullptr;
    DraftStore* draftStore_ = nullptr;
    AttemptSession* session_ = nullptr;
    UiSize* uiSize_ = nullptr;
    QString* providerId_ = nullptr;
    bool* draftRestoreChecked_ = nullptr;

    // ---- 控件引用：实际所有权在 Qt parent/child 对象树中 ----
    QLabel* practiceTitleLabel_ = nullptr;
    QLabel* practiceProgressLabel_ = nullptr;
    QLabel* questionLabel_ = nullptr;
    QScrollArea* questionScroll_ = nullptr;
    QWidget* questionContent_ = nullptr;
    QVBoxLayout* questionContentLayout_ = nullptr;
    QVBoxLayout* optionsLayout_ = nullptr;
    QButtonGroup* optionButtonGroup_ = nullptr;
    ui::MaterialCard* practiceMaterialCard_ = nullptr;
    QWidget* practiceControlBar_ = nullptr;
    QPushButton* previousQuestionButton_ = nullptr;
    QPushButton* nextQuestionButton_ = nullptr;
    QPushButton* questionListButton_ = nullptr;
    QPushButton* submitButton_ = nullptr;
    ui::QuestionNavigator* questionNavigator_ = nullptr;
    QFrame* submitConfirmationBubble_ = nullptr;
    QLabel* submitConfirmationLabel_ = nullptr;

    // ---- 只属于做题页的会话状态 ----
    int currentQuestionIndex_ = 0;
    int lockedPracticeViewportHeight_ = 0;

    friend class MainWindow;
};

}  // namespace quizpane
