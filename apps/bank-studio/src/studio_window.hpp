#pragma once

#include <QHash>
#include <QMainWindow>
#include "quizpane/studio/review_result.hpp"
#include <QJsonArray>
#include <QSet>
#include <QStringList>

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;
class QHBoxLayout;
class QTimer;
class QCloseEvent;

namespace quizpane::studio {
class GenerationWorkflow;
class SourceRowWidget;
class StyledDropdown;
using GeneratedBankCandidate = ReviewResult;
struct WorkflowProgress;
}

namespace quizpane::studio {

// 题库制作器的顶层页面控制器，只负责四步向导、文件选择和进度展示。规则引擎
// 是唯一的整理路径，全程离线，不涉及网络请求或模型厂商配置。
class StudioWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit StudioWindow(QWidget* parent = nullptr);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    QWidget* buildSourcePage();
    QWidget* buildProgressPage();
    QWidget* buildReviewPage();
    QWidget* buildFinishPage();
    QWidget* pageHeader(const QString& eyebrow, const QString& title,
                        const QString& description);
    void addSourceFiles();
    void appendSources(const QStringList& paths);
    void pairAnswer(const QString& question, const QString& answer);
    void removeSource(const QString& question);
    void updateNavigation();
    void movePage(int delta);
    void beginPreflight();
    void updateWorkflowProgress(const WorkflowProgress& progress);
    void populateReview(const GeneratedBankCandidate& candidate);
    void applyReviewFilter();
    void confirmRiskCategory(const QString& signal);
    void packageProvider();
    void applyStyle();

    QStackedWidget* pages_ = nullptr;
    QScrollArea* sourceScroll_ = nullptr;
    QVBoxLayout* sourceListLayout_ = nullptr;
    QWidget* sourcePanel_ = nullptr;
    QLabel* sourceSummary_ = nullptr;
    QLabel* modelSummary_ = nullptr;
    QLabel* phaseLabel_ = nullptr;
    QLabel* phaseDetail_ = nullptr;
    QLabel* activitySpinner_ = nullptr;
    QLabel* inputTokens_ = nullptr;
    QLabel* outputTokens_ = nullptr;
    QLabel* totalTokens_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QTreeWidget* reviewTree_ = nullptr;
    QPushButton* allReviewButton_ = nullptr;
    QPushButton* missingAnswerButton_ = nullptr;
    QPushButton* duplicateButton_ = nullptr;
    // "全部异常/缺少答案/疑似重复" 三个筛选按钮当前选中的过滤条件；空表示不过滤。
    QString activeReviewFilter_;
    // 复核页里按 riskLevel=soft 信号分组展示的批量确认区域，随每次 populateReview
    // 重建；数量、按钮和信号 key 一一对应，用于点击后批量勾选同类题目。
    QVBoxLayout* riskCategoryLayout_ = nullptr;
    QWidget* riskCategoryPanel_ = nullptr;
    QLabel* finishPath_ = nullptr;
    QLineEdit* bankName_ = nullptr;
    StyledDropdown* questionCount_ = nullptr;
    QPushButton* backButton_ = nullptr;
    QPushButton* nextButton_ = nullptr;
    QPushButton* startButton_ = nullptr;
    GenerationWorkflow* workflow_ = nullptr;
    QTimer* activityTimer_ = nullptr;
    int spinnerFrame_ = 0;
    QStringList sourcePaths_;
    QHash<QString, QString> answerPathsByQuestion_;
    QHash<QString, SourceRowWidget*> sourceRows_;
    QJsonArray generatedMaterials_;
    QJsonArray generatedQuestions_;
    QJsonArray reviewQuestions_;
    QHash<QString, QByteArray> generatedAssets_;
};

}  // namespace quizpane::studio
