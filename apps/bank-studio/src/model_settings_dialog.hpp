#pragma once

#include <QString>

#include <optional>

class QWidget;

namespace quizpane::studio {

// 模型设置页使用的值对象，作用接近 Java Web 中的表单 DTO。
// 对话框只在用户点击“保存”后返回新值；取消时返回 nullopt，因此调用方不需要
// 先修改成员再手工回滚。
struct ModelSettings {
    QString vendorId = QStringLiteral("openai");
    QString serviceName = QStringLiteral("OpenAI");
    QString modelName = QStringLiteral("gpt-5.2");
    QString endpoint = QStringLiteral("https://api.openai.com/v1");
    QString apiKey;
};

// 创建并同步执行模型设置对话框。网络请求和厂商差异全部封装在实现文件中，
// StudioWindow 只消费最终 DTO，不直接依赖 QNetworkReply 等基础设施类型。
[[nodiscard]] std::optional<ModelSettings> editModelSettings(
    QWidget* parent, const ModelSettings& current);

}  // namespace quizpane::studio
