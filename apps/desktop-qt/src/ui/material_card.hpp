#pragma once

#include <QString>
#include <QWidget>

class QLabel;
class QPushButton;
class QScrollArea;

namespace quizpane::ui {

// 共享材料卡片：展示在题干上方的可折叠面板。答题页和解析页各持有一份独立
// 实例（不跨页面共享控件），但复用同一份"按 materialId 判断是否需要重绘"
// 的逻辑，避免题组内切题时重复解析长文本 HTML 导致闪烁。
class MaterialCard final : public QWidget {
    Q_OBJECT
public:
    explicit MaterialCard(QWidget* parent = nullptr);

    // materialId 与上次显示的相同时直接返回，不重新设置内容、不重置折叠状态；
    // 不同（包括从空变为非空）时更新内容并展开。调用方应在每次切题时无脑调用，
    // 由这里自行判断是否需要重绘。
    void showMaterial(const QString& materialId, const QString& title,
                      const QString& contentHtml);
    void hideMaterial();
    QString currentMaterialId() const { return materialId_; }

private:
    void toggleCollapsed();
    void applyCollapsedState();

    QString materialId_;
    bool collapsed_ = false;
    QLabel* titleLabel_ = nullptr;
    QPushButton* toggleButton_ = nullptr;
    QScrollArea* bodyScroll_ = nullptr;
    QLabel* bodyLabel_ = nullptr;
};

}  // namespace quizpane::ui
