#include "../apps/desktop-qt/src/ui/material_card.hpp"

#include <QApplication>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QTemporaryDir>

// MaterialCard 是纯 UI 状态机：QT_QPA_PLATFORM=offscreen 下用真实 QApplication
// 驱动，断言可见性、折叠状态和"同一材料内切子题不重绘"的行为。不涉及网络或
// Provider，只测试阶段 3 新增的这一个控件本身。
int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    quizpane::ui::MaterialCard card;

    // 初始状态：未设置材料时不可见。
    if (card.isVisible()) return 1;

    // 材料一，第一次展示：默认展开、可见、记录 materialId。
    QTemporaryDir images;
    QPixmap sourceImage(640, 480);
    sourceImage.fill(Qt::red);
    const QString sourceImagePath = images.filePath(QStringLiteral("material.png"));
    if (!sourceImage.save(sourceImagePath)) return 13;
    card.resize(220, 300);
    card.show();
    card.showMaterial(QStringLiteral("material-001"), QStringLiteral("阅读材料一"),
                      QStringLiteral("<p>材料一正文</p>"),
                      QJsonArray{QUrl::fromLocalFile(sourceImagePath).toString()});
    app.processEvents();
    if (!card.isVisible() || card.currentMaterialId() != QStringLiteral("material-001")) return 2;
    auto* bodyScroll = card.findChild<QScrollArea*>(QStringLiteral("materialCardBodyScroll"));
    auto* toggleButton = card.findChild<QPushButton*>(QStringLiteral("materialCardToggle"));
    auto* bodyLabel = card.findChild<QLabel*>(QStringLiteral("materialCardBody"));
    if (!bodyScroll || !toggleButton || !bodyLabel) return 3;
    if (!bodyScroll->isVisible()) return 4;  // 默认展开
    if (bodyLabel->text() != QStringLiteral("<p>材料一正文</p>")) return 5;
    if (bodyLabel->width() > bodyScroll->viewport()->width()) return 14;
    auto* imagePreview = card.findChild<QPushButton*>(QStringLiteral("materialImagePreview"));
    if (!imagePreview || imagePreview->iconSize().width() > bodyScroll->viewport()->width()) return 15;

    // 用户手动折叠。
    toggleButton->click();
    if (bodyScroll->isVisible()) return 6;

    // 同一材料内切子题：materialId 不变，折叠状态必须保留，不重新解析 HTML。
    card.showMaterial(QStringLiteral("material-001"), QStringLiteral("阅读材料一"),
                      QStringLiteral("<p>不应该被使用的新内容</p>"));
    if (bodyScroll->isVisible()) return 7;  // 折叠状态保留
    if (bodyLabel->text() != QStringLiteral("<p>材料一正文</p>")) return 8;  // 内容未被重写

    // 切换到另一份材料：重新展开，内容更新。
    card.showMaterial(QStringLiteral("material-002"), QStringLiteral("阅读材料二"),
                      QStringLiteral("<p>材料二正文</p>"));
    if (card.currentMaterialId() != QStringLiteral("material-002")) return 9;
    if (!bodyScroll->isVisible()) return 10;  // 材料切换后重置为展开
    if (bodyLabel->text() != QStringLiteral("<p>材料二正文</p>")) return 11;

    // 切到独立题：隐藏整张卡片。
    card.hideMaterial();
    if (card.isVisible() || !card.currentMaterialId().isEmpty()) return 12;

    return 0;
}
