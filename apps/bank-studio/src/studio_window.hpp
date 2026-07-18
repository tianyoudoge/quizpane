#pragma once

#include <QHash>
#include <QImage>
#include <QMainWindow>
#include "model_settings_dialog.hpp"
#include "quizpane/studio/review_result.hpp"
#include <QJsonArray>
#include <QList>
#include <QSet>
#include <QStringList>

class QLabel;
class QCheckBox;
class QLineEdit;
class QPlainTextEdit;
class QTextEdit;
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
class ModelClient;
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
    void showReviewQuestion(QTreeWidgetItem* item);
    bool saveCurrentReviewQuestion();
    void confirmCurrentReviewQuestion();
    void excludeCurrentReviewQuestion();
    void addManualMaterialUnderline();
    void displayReviewAssets(const QList<QJsonObject>& assets);
    void recropReviewAsset(const QJsonObject& asset);
    void requestAiCrop(const QJsonObject& asset);
    void handleAiCropResult(const QString& rawText, const QString& error);
    bool commitReviewCrop(const QJsonObject& asset, const QImage& page,
                          const QRectF& normalizedCrop);
    void setReviewOptions(const QJsonArray& options);
    QJsonArray reviewOptions() const;
    void addReviewOption(const QString& id = {}, const QString& text = {});
    void requestAiReview();
    void handleAiReviewResult(const QString& rawText, const QString& error);
    void editModelSettings();
    void showDonationDialog();
    bool ensureModelApiKeyLoaded();
    void updateAiReviewAffordance();
    void updateReviewStemHeight();
    void packageProvider();
    void applyStyle();

    QStackedWidget* pages_ = nullptr;
    QScrollArea* sourceScroll_ = nullptr;
    QVBoxLayout* sourceListLayout_ = nullptr;
    QWidget* sourcePanel_ = nullptr;
    QLabel* sourceSummary_ = nullptr;
    QCheckBox* hasAnswerKeyCheck_ = nullptr;
    QLabel* modelSummary_ = nullptr;
    QLabel* phaseLabel_ = nullptr;
    QLabel* phaseDetail_ = nullptr;
    QLabel* activitySpinner_ = nullptr;
    QLabel* inputTokens_ = nullptr;
    QLabel* outputTokens_ = nullptr;
    QLabel* totalTokens_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QTreeWidget* reviewTree_ = nullptr;
    QLabel* reviewDetailTitle_ = nullptr;
    QLabel* reviewDetailStatus_ = nullptr;
    QLabel* reviewStemLabel_ = nullptr;
    QTextEdit* reviewStemEditor_ = nullptr;
    QWidget* reviewQuestionEditorPanel_ = nullptr;
    QWidget* reviewOptionsPanel_ = nullptr;
    QVBoxLayout* reviewOptionsLayout_ = nullptr;
    QList<QLineEdit*> reviewOptionEditors_;
    QLabel* reviewAnswerLabel_ = nullptr;
    QLineEdit* reviewAnswerEditor_ = nullptr;
    QLabel* reviewSolutionLabel_ = nullptr;
    QPlainTextEdit* reviewSolutionEditor_ = nullptr;
    QWidget* reviewVisualPanel_ = nullptr;
    QVBoxLayout* reviewVisualLayout_ = nullptr;
    QPushButton* saveReviewButton_ = nullptr;
    QPushButton* confirmReviewButton_ = nullptr;
    QPushButton* excludeReviewButton_ = nullptr;
    QPushButton* aiReviewButton_ = nullptr;
    QTreeWidgetItem* currentReviewItem_ = nullptr;
    QTreeWidgetItem* currentMaterialItem_ = nullptr;
    QPushButton* manualMaterialUnderlineButton_ = nullptr;
    bool aiReviewInFlight_ = false;
    bool aiCropInFlight_ = false;
    QJsonObject pendingCropAsset_;
    QImage pendingCropPage_;
    QRectF pendingCropContext_;
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
    ModelClient* modelClient_ = nullptr;
    ModelSettings modelSettings_;
    QTimer* activityTimer_ = nullptr;
    int spinnerFrame_ = 0;
    QStringList sourcePaths_;
    QHash<QString, QString> answerPathsByQuestion_;
    QHash<QString, SourceRowWidget*> sourceRows_;
    QJsonArray generatedMaterials_;
    QJsonArray generatedQuestions_;
    QJsonArray reviewQuestions_;
    QHash<QString, QByteArray> generatedAssets_;
    bool generatedHasAnswerKey_ = true;
};

}  // namespace quizpane::studio
