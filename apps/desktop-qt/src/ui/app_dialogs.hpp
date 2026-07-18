#pragma once

#include <QKeySequence>
#include "quizpane/draft_store.hpp"

#include <optional>

class QWidget;

namespace quizpane::ui {

enum class DraftChoice { Restore, Later, Discard };
struct DraftDecision {
    DraftChoice choice = DraftChoice::Later;
    int index = -1;
};

// 弹出老板键编辑窗口。返回 nullopt 表示用户取消，类似 Web 表单关闭时不提交。
std::optional<QKeySequence> askBossKey(QWidget* parent,
                                       const QKeySequence& current);

// 展示只读“关于”窗口。窗口内容从 QApplication 的全局元数据中读取，
// 这样版本号只有一处事实来源，避免界面和构建产物不一致。
void showAbout(QWidget* parent);

// 统一展示赞赏码。当前二维码是明确标示的非支付占位资源，正式发布时仅替换资源文件。
void showDonation(QWidget* parent);

DraftDecision chooseDraft(QWidget* parent, const QList<DraftSnapshot>& drafts);

bool confirm(QWidget* parent, const QString& title, const QString& message,
             const QString& confirmText, const QString& cancelText);

}  // namespace quizpane::ui
