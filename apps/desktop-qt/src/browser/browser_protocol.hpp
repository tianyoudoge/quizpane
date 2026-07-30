#pragma once

#include <QJsonObject>
#include <QString>

namespace quizpane::browser::protocol {

inline constexpr int kVersion = 1;
inline constexpr qsizetype kMaxMessageBytes = 16 * 1024;

// 协议仅传递浏览器窗口与 HTML5 播放器的控制状态；不承载网页内容、Cookie 或
// 视频地址。把解析放在独立模块中，网络边界能单元测试而无需启动 WebSocket 服务。
bool parseMessage(const QString& text, QJsonObject* message, QString* error = nullptr);
bool isExtensionMessageType(const QString& type);
bool isDesktopCommandType(const QString& type);
QJsonObject makeMessage(const QString& type, const QJsonObject& payload = {},
                        const QString& requestId = {});

}  // namespace quizpane::browser::protocol
