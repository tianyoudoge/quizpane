#pragma once

#include <QFrame>
#include <QString>

class QLabel;
class QPushButton;
class QWidget;

namespace quizpane::studio {

// 已导入题目资料在向导第一步的一行展示：左侧是题目文件信息，右侧根据是否
// 已配对答案/解析文档切换成两种状态。行本身只负责展示和发出信号，题目与
// 答案的配对关系仍由 StudioWindow 统一持有（sourcePaths_/answerPathsByQuestion_），
// 避免 UI 行控件和向导状态机互相拥有对方的数据。
class SourceRowWidget final : public QFrame {
    Q_OBJECT
public:
    SourceRowWidget(const QString& questionPath, QWidget* parent = nullptr);

    QString questionPath() const { return questionPath_; }

    void setPairedAnswer(const QString& answerPath);
    void clearPairedAnswer();

signals:
    void answerRequested();
    void answerDropped(const QString& answerPath);
    void answerCleared();
    void removeRequested();

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    QString questionPath_;
    QLabel* pairedAnswerLabel_ = nullptr;
    QPushButton* addAnswerButton_ = nullptr;
    QPushButton* clearAnswerButton_ = nullptr;
};

}  // namespace quizpane::studio
