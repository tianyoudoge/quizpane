#pragma once

#include "model_settings_dialog.hpp"

#include <QHash>
#include <QMainWindow>
#include "quizpane/studio/review_result.hpp"
#include <QJsonArray>
#include <QStringList>

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QTreeWidget;
class QVBoxLayout;
class QTimer;
class QCloseEvent;
class QNetworkAccessManager;

namespace quizpane::studio {
class GenerationWorkflow;
class SourceRowWidget;
class StyledDropdown;
using GeneratedBankCandidate = ReviewResult;
struct WorkflowProgress;
}

namespace quizpane::studio {

// 题库制作器的顶层页面控制器，只负责四步向导、文件选择和进度展示。
// 模型厂商协议与网络请求放在 model_settings_dialog 中，避免 UI Controller
// 同时承担 Adapter/Service 职责。Qt 控件仍由父子对象树托管生命周期。
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
    void showModelSettings();
    void updateNavigation();
    void movePage(int delta);
    void beginPreflight();
    void updateWorkflowProgress(const WorkflowProgress& progress);
    void populateReview(const GeneratedBankCandidate& candidate);
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
    QLabel* finishPath_ = nullptr;
    QLineEdit* bankName_ = nullptr;
    StyledDropdown* questionCount_ = nullptr;
    StyledDropdown* generationMode_ = nullptr;
    QPushButton* backButton_ = nullptr;
    QPushButton* nextButton_ = nullptr;
    QPushButton* startButton_ = nullptr;
    QNetworkAccessManager* networkManager_ = nullptr;
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
    // 一个 DTO 保存完整模型选择，避免 vendor/model/endpoint 多个平行字段只更新
    // 其中一部分。API Key 当前只存在进程内，不进入普通配置文件。
    ModelSettings modelSettings_;
};

}  // namespace quizpane::studio
