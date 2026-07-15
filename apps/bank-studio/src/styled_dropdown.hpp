#pragma once

#include <QWidget>
#include <QIcon>
#include <QString>
#include <QVariant>
#include <QVector>

class QLabel;
class QFrame;
class QListWidget;

namespace quizpane::studio {

// 自定义样式下拉框，替代非可编辑场景下的 QComboBox：外观完全由 QSS 接管，
// 弹层内部复用 QListWidget 换取免费的键盘导航/悬停高亮/滚动，避免手搓整
// 套 item 视图。仅覆盖单选、不可编辑场景；可编辑输入的下拉（如模型名称）
// 仍使用原生 QComboBox。
class StyledDropdown final : public QWidget {
    Q_OBJECT
public:
    explicit StyledDropdown(QWidget* parent = nullptr);

    void addItem(const QString& text, const QVariant& data = {});
    void addItem(const QIcon& icon, const QString& text, const QVariant& data = {});
    void addItems(const QStringList& texts);
    void clear();

    int currentIndex() const { return currentIndex_; }
    void setCurrentIndex(int index);
    QVariant currentData() const;
    QString currentText() const;
    int findData(const QVariant& data) const;

signals:
    void currentIndexChanged(int index);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    struct Entry {
        QIcon icon;
        QString text;
        QVariant data;
    };

    void ensurePopup();
    void showPopup();
    void refreshLabel();
    void selectRow(int row);

    QVector<Entry> entries_;
    int currentIndex_ = -1;
    QLabel* currentLabel_ = nullptr;
    QLabel* arrow_ = nullptr;
    QFrame* popup_ = nullptr;
    QListWidget* popupList_ = nullptr;
};

}  // namespace quizpane::studio
