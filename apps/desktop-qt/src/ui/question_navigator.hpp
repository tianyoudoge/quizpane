#pragma once

#include <QFrame>
#include <QSet>
#include <QVector>

class QLabel;
class QPushButton;
class QGridLayout;

namespace quizpane::ui {

// 答题页底栏弹出的题号清单。组件只接收题目数量、已答集合和当前位置，
// 不持有 Provider 或答题数据，因而单选、多选都由 MainWindow 统一判定。
class QuestionNavigator final : public QFrame {
    Q_OBJECT

public:
    explicit QuestionNavigator(QWidget* parent = nullptr);

    void setState(int questionCount, const QSet<int>& answered, int currentIndex);
    QPushButton* questionButton(int index) const;

signals:
    void questionSelected(int index);

private:
    void rebuild(int questionCount);
    void refreshButtonStyle(QPushButton* button, bool answered, bool current);

    QLabel* summaryLabel_ = nullptr;
    QGridLayout* grid_ = nullptr;
    QVector<QPushButton*> buttons_;
};

}  // namespace quizpane::ui
