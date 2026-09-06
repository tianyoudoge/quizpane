#pragma once

#include <QJsonArray>
#include <QString>

#include <functional>

class QVBoxLayout;
class QWidget;
class QStackedWidget;
class QLabel;

namespace quizpane {
class ProviderLoader;

// 目录页（分类与题量选择）的控件和交互逻辑。
// 非 QObject 类，由 MainWindow 作为值成员持有。
class CatalogPageController {
public:
    CatalogPageController() = default;

    // 构造后、buildInto() 前必须先调用一次。
    // loginPage/loginTitleLabel/loginDetailLabel：requestCatalog() 在目录数据
    // 到达前，复用登录页的标题/详情标签展示加载状态（拆分前的既有行为，
    // 登录->目录切换期间页面尚未跳转，这两个控件仍属于 MainWindow 的登录页）。
    // onApplyUiSize：页面切换后需要重新套用当前窗口尺寸偏好（原 applyUiSize(uiSize_)
    // 调用），该逻辑涉及卡片样式和图标等纯 MainWindow 状态，仍由 MainWindow 提供。
    void init(QWidget* catalogPage, QStackedWidget* pages, ProviderLoader& provider,
              QWidget* loginPage, QLabel* loginTitleLabel, QLabel* loginDetailLabel,
              std::function<void()> onApplyUiSize);

    // 构建目录页内部控件并接入 catalogPage_；调用前必须先调用 init()。
    void buildInto();

    void requestCatalog();
    // startAttempt 由 MainWindow 提供的回调触发（practicePage 归属于
    // PracticePageController，本控制器不直接依赖它）。
    void populateCatalog(const QJsonArray& nodes,
                         const std::function<void(const QString&, const QString&, int, bool)>& onStartAttempt);
    void returnToCatalog();

private:
    QWidget* catalogPage_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    ProviderLoader* provider_ = nullptr;
    QWidget* loginPage_ = nullptr;
    QLabel* loginTitleLabel_ = nullptr;
    QLabel* loginDetailLabel_ = nullptr;
    std::function<void()> onApplyUiSize_;

    QVBoxLayout* catalogListLayout_ = nullptr;

    friend class MainWindow;
};

}  // namespace quizpane
