#pragma once

#include "quizpane/studio/generation_workflow.hpp"

#include <QFrame>
#include <QString>

class QLabel;
class QComboBox;
class QPushButton;
class QWidget;

namespace quizpane::studio {

// 已导入题目资料在向导第一步的一行展示：答案策略默认自动检测，也允许用户
// 强制指定含答案/无答案；配对答案文档时自动切到“含答案”。行本身只负责
// 展示和发出信号，题目与
// 答案的配对关系仍由 StudioWindow 统一持有（sourcePaths_/answerPathsByQuestion_），
// 避免 UI 行控件和向导状态机互相拥有对方的数据。
class SourceRowWidget final : public QFrame {
    Q_OBJECT
public:
    SourceRowWidget(const QString& questionPath, QWidget* parent = nullptr);

    QString questionPath() const { return questionPath_; }

    AnswerPolicyHint answerPolicy() const;
    void setAnswerPolicy(AnswerPolicyHint policy);
    void setPairedAnswer(const QString& answerPath);
    void clearPairedAnswer();

signals:
    void answerPolicyChanged(quizpane::studio::AnswerPolicyHint policy);
    void answerRequested();
    void answerDropped(const QString& answerPath);
    void answerCleared();
    void removeRequested();

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    QString questionPath_;
    QComboBox* answerLocation_ = nullptr;
    QLabel* pairedAnswerLabel_ = nullptr;
    QPushButton* addAnswerButton_ = nullptr;
    QPushButton* clearAnswerButton_ = nullptr;
};

}  // namespace quizpane::studio
