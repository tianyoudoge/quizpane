#pragma once

#include <QString>

#include <optional>

class QWidget;

namespace quizpane::studio {

// MinerU 云解析设置。作用接近 Java Web 中的表单 DTO：对话框只在用户点击"保存"
// 后返回新值，取消时返回 nullopt，调用方不必先改成员再手工回滚。
//
// 只有一个 Token 需要配置。留空时制作器仍然可用——只是只走本地解析，复杂版面
// （扫描件、图表、图片选项）的识别率会下降。
struct MineruConfig {
    QString token;
    // 默认选用 MinerU 推荐的准确识别；兼容模式只在识别异常时使用。
    QString modelVersion = QStringLiteral("vlm");
    // 保留给协议层；界面不再把 OCR 作为全局开关，由智能解析自动判断。
    bool isOcr = false;
    // 智能解析是新用户的推荐方式。没有 Token 时会在实际整理前提示配置，
    // 不会静默上传任何文件。
    bool cloudEnabled = true;
};

// 创建并同步执行 MinerU 设置对话框。
[[nodiscard]] std::optional<MineruConfig> editMineruSettings(QWidget* parent,
                                                             const MineruConfig& current);

}  // namespace quizpane::studio
