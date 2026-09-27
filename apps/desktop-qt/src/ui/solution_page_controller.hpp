#pragma once

#include "attempt_session.hpp"
#include "ui_size.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <functional>

class QFrame;
class QLabel;
class QTimer;
class QWidget;
class QVBoxLayout;
class QPushButton;
class QStackedWidget;

namespace quizpane {
class AiExplainService;
class ProviderLoader;
QString formatAiResponse(const QString& raw);
namespace ui {
class MaterialCard;
}

// 解析页（解题结果展示）的控件、状态和交互逻辑。
// 非 QObject 类，由 MainWindow 作为值成员持有；不持有 MainWindow*，
// 跨页面共享状态（当前作答会话）通过 AttemptSession& 引用访问。
class SolutionPageController {
public:
    SolutionPageController() = default;

    // 构造后、buildInto() 前必须先调用一次。
    void init(QWidget* solutionPage, QStackedWidget* pages, QWidget* catalogPage,
              ProviderLoader& provider, AttemptSession& session, UiSize& uiSize,
              std::function<void()> onApplyUiSize);

    // 构建解析页内部控件并接入 solutionPage_；调用前必须先调用 init()。
    void buildInto();

    void requestResults();
    void showSolution(int index);
    void exportAttemptResults();
    void setResultSummary(const QString& text);
    void setNoSolutionsMessage();

    int currentSolutionIndex() const { return currentSolutionIndex_; }
    int solutionCount() const { return static_cast<int>(session_->solutions.size()); }
    bool isCurrentPage() const;
    QWidget* controlBar() const { return solutionControlBar_; }

private:
    QWidget* solutionPage_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QWidget* catalogPage_ = nullptr;
    ProviderLoader* provider_ = nullptr;
    AttemptSession* session_ = nullptr;
    UiSize* uiSize_ = nullptr;
    std::function<void()> onApplyUiSize_;

    // ---- 控件引用：实际所有权在 Qt parent/child 对象树中 ----
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
    ui::MaterialCard* solutionMaterialCard_ = nullptr;
    QVBoxLayout* solutionContentLayout_ = nullptr;

    QPushButton* aiExplainButton_ = nullptr;
    QPushButton* aiConfigButton_ = nullptr;
    QFrame* aiExplainPanel_ = nullptr;
    QLabel* aiExplainContentLabel_ = nullptr;
    AiExplainService* aiService_ = nullptr;
    QTimer* aiSpinnerTimer_ = nullptr;
    int aiSpinnerFrame_ = 0;

    // ---- 只属于解析页的会话状态 ----
    int currentSolutionIndex_ = 0;

    void onAiExplainClicked();
    bool showAiConfigDialog();

    friend class MainWindow;
};

}  // namespace quizpane
