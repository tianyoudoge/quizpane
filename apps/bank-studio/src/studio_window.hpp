#pragma once

#include <QHash>
#include <QMainWindow>
#include "quizpane/studio/generation_workflow.hpp"
#include "quizpane/studio/review_result.hpp"
#include "review/review_page_controller.hpp"
#include "ui/mineru_settings_dialog.hpp"
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QStringList>

#include <optional>

class QLabel;
class QCheckBox;
class QFrame;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QScrollArea;
class QStackedWidget;
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
    QWidget* buildFinishPage();
    QWidget* pageHeader(const QString& eyebrow, const QString& title,
                        const QString& description);
    void addSourceFiles();
    void appendSources(const QStringList& paths);
    void pairAnswer(const QString& question, const QString& answer);
    void removeSource(const QString& question);
    void updateNavigation();
    void startFromSources();
    void handleBackNavigation();
    void movePage(int delta);
    void beginPreflight();
    void discardPreviousGenerationForNewTask();
    void startCloudParseThenGenerate(const QList<SourceMaterialGroup>& groups);
    bool shouldUseCloudParse() const;
    void processNextCloudSource();
    void offerCloudTaskResume();
    void persistCloudTask();
    void clearPersistedCloudTask(bool removeCachedResults = true);
    void updateMineruConfigSummary();
    void updateWorkflowProgress(const WorkflowProgress& progress);
    void populateReview(const GeneratedBankCandidate& candidate);
    void applyReviewFilter();
    void showReviewQuestion(QTreeWidgetItem* item);
    bool saveCurrentReviewQuestion();
    bool reviewQuestionIsDirty() const;
    bool commitOpenReviewQuestion(const QString& consequence);
    void confirmCurrentReviewQuestion();
    void excludeCurrentReviewQuestion();
    void refreshReviewDecisionState();
    void advanceToNextReviewIssue();
    void addManualMaterialUnderline();
    QByteArray ensureReviewAssetBytes(const QJsonObject& asset);
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
    QPushButton* mineruConfigButton_ = nullptr;
    QLabel* mineruConfigSummary_ = nullptr;
    QLabel* sourceModeHint_ = nullptr;
    QFrame* parseStatusChip_ = nullptr;
    QLabel* parseStatusText_ = nullptr;
    QLabel* phaseLabel_ = nullptr;
    QLabel* phaseDetail_ = nullptr;
    QLabel* activitySpinner_ = nullptr;
    QLabel* sourceCount_ = nullptr;
    QLabel* generatedCount_ = nullptr;
    QLabel* reviewCount_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QLabel* progressStatus_ = nullptr;
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
    // 已提交的云端任务会落到 QSettings（Token 不在其中）。结果 ZIP 仅缓存于
    // AppLocalDataLocation，任务完成或用户选择不再等待后立即清理。
    QString cloudSessionId_;
    QString cloudCacheDir_;
    QString cloudBatchId_;
    QList<SourceMaterialGroup> pendingGroups_;
    int cloudIndex_ = 0;
    bool cloudParsingAnswer_ = false;
    QTimer* activityTimer_ = nullptr;
    int spinnerFrame_ = 0;
    QStringList sourcePaths_;
    QHash<QString, QString> answerPathsByQuestion_;
    QHash<QString, AnswerPolicyHint> answerPolicyByQuestion_;
    QHash<QString, SourceRowWidget*> sourceRows_;

    // 第三步复核页及其所有控件、数据和逻辑。
    ReviewPageController reviewController_;
};

}  // namespace quizpane::studio
