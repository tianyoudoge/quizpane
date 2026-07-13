#pragma once

#include <QMainWindow>
#include <QStringList>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QTableWidget;
class QTimer;

namespace quizpane::studio {

class StudioWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit StudioWindow(QWidget* parent = nullptr);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    QWidget* buildSourcePage();
    QWidget* buildProgressPage();
    QWidget* buildReviewPage();
    QWidget* buildFinishPage();
    QWidget* pageHeader(const QString& eyebrow, const QString& title,
                        const QString& description);
    void addSourceFiles();
    void appendSources(const QStringList& paths);
    void removeSelectedSource();
    void showModelSettings();
    void updateNavigation();
    void movePage(int delta);
    void beginPreflight();
    void runPreflightStep();
    void applyStyle();

    QStackedWidget* pages_ = nullptr;
    QListWidget* sourceList_ = nullptr;
    QWidget* sourcePanel_ = nullptr;
    QLabel* sourceSummary_ = nullptr;
    QLabel* modelSummary_ = nullptr;
    QLabel* phaseLabel_ = nullptr;
    QLabel* phaseDetail_ = nullptr;
    QLabel* inputTokens_ = nullptr;
    QLabel* outputTokens_ = nullptr;
    QLabel* totalTokens_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QTableWidget* reviewTable_ = nullptr;
    QLabel* finishPath_ = nullptr;
    QPushButton* backButton_ = nullptr;
    QPushButton* nextButton_ = nullptr;
    QPushButton* startButton_ = nullptr;
    QTimer* preflightTimer_ = nullptr;
    QStringList sourcePaths_;
    int preflightStep_ = 0;
    qint64 estimatedInputTokens_ = 0;
    QString modelService_ = QStringLiteral("OpenAI");
    QString modelName_ = QStringLiteral("自动选择（推荐）");
    QString modelEndpoint_ = QStringLiteral("https://api.openai.com/v1");
};

}  // namespace quizpane::studio
