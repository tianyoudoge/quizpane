#pragma once

#include <QHash>
#include <QImage>
#include <QMainWindow>
#include "mineru_settings_dialog.hpp"
#include "quizpane/studio/generation_workflow.hpp"
#include "quizpane/studio/review_result.hpp"
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QStringList>

#include <QTemporaryDir>

#include <memory>

class QLabel;
class QButtonGroup;
class QCheckBox;
class QFrame;
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
class QNetworkAccessManager;

namespace quizpane::studio {
class GenerationWorkflow;
class MineruExtractionJob;
class SourceRowWidget;
class StyledDropdown;
using GeneratedBankCandidate = ReviewResult;
struct WorkflowProgress;
}

namespace quizpane::studio {

// 题库制作器的顶层页面控制器，只负责四步向导、文件选择和进度展示。规则引擎
// 是本地默认整理路径；用户明确启用时，可先通过 MinerU 云解析统一生成抽取结果。
class StudioWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit StudioWindow(QWidget* parent = nullptr);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    friend class StudioWindowReviewTest;
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
    void startCloudParseThenGenerate(const QList<SourceMaterialGroup>& groups);
    bool shouldUseCloudParse() const;
    void processNextCloudSource();
    void updateWorkflowProgress(const WorkflowProgress& progress);
    void populateReview(const GeneratedBankCandidate& candidate);
    void applyReviewFilter();
    void confirmRiskCategory(const QString& signal);
    void showReviewQuestion(QTreeWidgetItem* item);
    bool saveCurrentReviewQuestion();
    bool reviewQuestionIsDirty() const;
    bool commitOpenReviewQuestion(const QString& consequence);
    void confirmCurrentReviewQuestion();
    void excludeCurrentReviewQuestion();
    void addManualMaterialUnderline();
    void displayReviewAssets(const QList<QJsonObject>& assets);
    void recropReviewAsset(const QJsonObject& asset);
    bool commitReviewCrop(const QJsonObject& asset, const QImage& page,
                          const QRectF& normalizedCrop);
    void setReviewOptions(const QJsonArray& options);
    QJsonArray reviewOptions() const;
    void addReviewOption(const QString& id = {}, const QString& text = {});
    // 返回 true 仅表示用户确认并成功保存了配置；取消或保存失败都不改变调用方流程。
    bool editMineruSettings(const QString& notice = {});
    void updateParseModeSummary();
    void editParseModeSettings();
    void selectParseMode(bool cloud);
    void showDonationDialog();
    void showFeedbackDialog();
    void updateReviewStemHeight();
    void packageProvider();
    void applyStyle();

    QStackedWidget* pages_ = nullptr;
    QScrollArea* sourceScroll_ = nullptr;
    QVBoxLayout* sourceListLayout_ = nullptr;
    QWidget* sourcePanel_ = nullptr;
    QLabel* sourceSummary_ = nullptr;
    QLabel* parseModeSummary_ = nullptr;
    QFrame* parseModeCard_ = nullptr;
    QPushButton* ruleModeCard_ = nullptr;
    QPushButton* smartModeCard_ = nullptr;
    QFrame* parseStatusChip_ = nullptr;
    QLabel* parseStatusText_ = nullptr;
    QLabel* phaseLabel_ = nullptr;
    QLabel* phaseDetail_ = nullptr;
    QLabel* activitySpinner_ = nullptr;
    QLabel* sourceCount_ = nullptr;
    QLabel* generatedCount_ = nullptr;
    QLabel* reviewCount_ = nullptr;
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
    QTreeWidgetItem* currentReviewItem_ = nullptr;
    QTreeWidgetItem* currentMaterialItem_ = nullptr;
    QPushButton* manualMaterialUnderlineButton_ = nullptr;
    QJsonObject pendingCropAsset_;
    QImage pendingCropPage_;
    QPushButton* allReviewButton_ = nullptr;
    QPushButton* allQuestionsButton_ = nullptr;
    QButtonGroup* reviewFilterGroup_ = nullptr;
    QPushButton* missingAnswerButton_ = nullptr;
    QPushButton* duplicateButton_ = nullptr;
    // 顶部分类与风险类别共用一个互斥组；空条件明确对应“全部题目”。
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
    QNetworkAccessManager* networkManager_ = nullptr;
    // 只保存非敏感配置；Token 始终按需从系统钥匙串读取，不驻留在窗口对象里。
    MineruConfig mineruConfig_;
    MineruExtractionJob* mineruJob_ = nullptr;
    // 云解析中间产物只存活于本次整理：QTemporaryDir 析构时自动清理，避免用户
    // 材料的副本长期留在磁盘上。
    std::unique_ptr<QTemporaryDir> cloudTempDir_;
    QList<SourceMaterialGroup> pendingGroups_;
    int cloudIndex_ = 0;
    bool cloudParsingAnswer_ = false;
    QTimer* activityTimer_ = nullptr;
    int spinnerFrame_ = 0;
    QStringList sourcePaths_;
    QHash<QString, QString> answerPathsByQuestion_;
    QHash<QString, bool> hasAnswerKeyByQuestion_;
    QHash<QString, SourceRowWidget*> sourceRows_;
    QJsonArray generatedMaterials_;
    QJsonArray generatedQuestions_;
    QJsonArray reviewQuestions_;
    QHash<QString, QByteArray> generatedAssets_;
    bool generatedHasAnswerKey_ = true;
};

}  // namespace quizpane::studio
