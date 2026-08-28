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
    // 官方推荐 vlm；pipeline 作为对照与降级选项。
    QString modelVersion = QStringLiteral("vlm");
    // 扫描件需要 OCR。文字型 PDF 保持关闭，避免不必要的识别误差。
    bool isOcr = false;
    // 是否允许把文档上传到 MinerU 云服务。默认关闭：上传属于把用户材料交给
    // 第三方，必须由用户显式同意，不能靠"填了 Token"隐式推断。
    bool cloudEnabled = false;
};

// 创建并同步执行 MinerU 设置对话框。
[[nodiscard]] std::optional<MineruConfig> editMineruSettings(QWidget* parent,
                                                             const MineruConfig& current);

}  // namespace quizpane::studio
