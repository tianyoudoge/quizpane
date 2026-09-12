#pragma once

#include "review_image_utils.hpp"
#include "quizpane/studio/review_result.hpp"

#include <QButtonGroup>
#include <QCache>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <functional>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QTextEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;
class QWidget;

namespace quizpane::studio {
using GeneratedBankCandidate = ReviewResult;
}

namespace quizpane::studio {

// 复核页（第三步）的所有控件、数据和逻辑。
// 非 QObject 类，由 StudioWindow 作为值成员持有。
// StudioWindow 通过 reviewController_ 访问这里的字段与方法。
class ReviewPageController {
public:
    ReviewPageController() = default;

    // 构造后、buildPage() 前必须先调用一次。
    // parentForDialogs: 弹窗的父窗口（StudioWindow 本身）。
    // sourcePaths: 指向 StudioWindow::sourcePaths_ 的只读引用，懒渲染时读取原文件路径。
    // onUpdateNavigation: 装载结果或决策变化后回传剩余复核数，更新主窗口统计与导航。
    void init(QWidget* parentForDialogs, const QStringList& sourcePaths,
              std::function<void(int)> onUpdateNavigation);

    // 构建并返回第三步复核页 QWidget。调用前必须先调用 init()。
    QWidget* buildPage();

    // ---- 公开给 StudioWindow 的 review 方法（通过 reviewController_ 调用）----
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
    void updateReviewStemHeight();

    // 新任务前清理本控制器拥有的所有状态（由 StudioWindow::discardPreviousGenerationForNewTask 调用）。
    void discardForNewTask();

    // ---- 控件指针（实际所有权在 Qt parent/child 对象树中）----
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
    QString activeReviewFilter_;
    QVBoxLayout* riskCategoryLayout_ = nullptr;
    QWidget* riskCategoryPanel_ = nullptr;
    QLabel* reviewSummary_ = nullptr;

    // ---- 数据 ----
    QJsonArray generatedMaterials_;
    QJsonArray generatedQuestions_;
    QJsonArray reviewQuestions_;
    QHash<QString, QByteArray> generatedAssets_;
    QHash<QString, QJsonObject> reviewSourceImages_;
    QHash<QString, QByteArray> reviewAssets_;
    QCache<QString, QByteArray> lazyReviewAssets_{16 * 1024}; // KiB; rebuildable only
    ReviewPdfCache reviewPdfCache_;
    bool generatedHasAnswerKey_ = true;

private:
    QWidget* parentWidget_ = nullptr;
    const QStringList* sourcePaths_ = nullptr;
    std::function<void(int)> onUpdateNavigation_;

    friend class StudioWindowReviewTest;
};

}  // namespace quizpane::studio
