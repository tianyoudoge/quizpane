#pragma once

#include <QKeySequence>

#include <optional>

class QWidget;

namespace quizpane::ui {

// 弹出老板键编辑窗口。返回 nullopt 表示用户取消，类似 Web 表单关闭时不提交。
std::optional<QKeySequence> askBossKey(QWidget* parent,
                                       const QKeySequence& current);

// 展示只读“关于”窗口。窗口内容从 QApplication 的全局元数据中读取，
// 这样版本号只有一处事实来源，避免界面和构建产物不一致。
void showAbout(QWidget* parent);

}  // namespace quizpane::ui
