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
    QString modelVendorId_ = QStringLiteral("openai");
    QString modelService_ = QStringLiteral("OpenAI");
    // 初次打开时使用 OpenAI 的内置兜底模型；成功获取远端列表后，用户可改选
    // 当前账号真正可用的模型。这里不能写一个并不存在的“自动选择”占位值。
    QString modelName_ = QStringLiteral("gpt-5.2");
    QString modelEndpoint_ = QStringLiteral("https://api.openai.com/v1");
    // 当前只保存在生成器进程内，绝不写入普通配置文件。后续模型调用落地时，
    // 再接入 Keychain/Credential Manager/Secret Service 持久化。
    QString modelApiKey_;
};

}  // namespace quizpane::studio
