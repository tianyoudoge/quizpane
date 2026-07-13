#pragma once

#include <QIcon>

namespace quizpane::ui {

// 轻量线性图标的语义名称。这里不用图片资源或第三方图标库，是为了让
// 低配置机器减少资源加载，同时保证所有平台的线条、颜色完全一致。
enum class LineIcon {
    Previous,
    Next,
    Submit,
    Pin,
    Resize,
    Close,
    Catalog,
    Menu,
    ChevronUp,
    ChevronDown
};

// 可以类比 Web 前端中的“图标组件”：调用方只传语义，函数负责绘制 24x24 图标。
QIcon makeLineIcon(LineIcon type);

}  // namespace quizpane::ui
