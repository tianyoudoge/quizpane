#include "browser_protocol.hpp"

#include <QDateTime>
#include <QJsonDocument>
#include <QUuid>

namespace quizpane::browser::protocol {
namespace {

void setError(QString* error, const QString& text) {
    if (error) *error = text;
}

}  // namespace

bool parseMessage(const QString& text, QJsonObject* message, QString* error) {
    if (!message) {
        setError(error, QStringLiteral("消息接收对象为空"));
        return false;
    }
    if (text.toUtf8().size() > kMaxMessageBytes) {
        setError(error, QStringLiteral("浏览器消息超过大小限制"));
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8(), &parseError);
    if (!document.isObject()) {
        setError(error, QStringLiteral("浏览器消息不是 JSON 对象"));
        return false;
    }
    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("protocolVersion")).toInt(-1) != kVersion) {
        setError(error, QStringLiteral("不支持的浏览器协议版本"));
        return false;
    }
    if (object.value(QStringLiteral("type")).toString().isEmpty()) {
        setError(error, QStringLiteral("浏览器消息缺少类型"));
        return false;
    }
    if (!object.value(QStringLiteral("payload")).isUndefined() &&
        !object.value(QStringLiteral("payload")).isObject()) {
        setError(error, QStringLiteral("浏览器消息 payload 必须为对象"));
        return false;
    }
    *message = object;
    return true;
}

bool isExtensionMessageType(const QString& type) {
    return type == QStringLiteral("hello") || type == QStringLiteral("ping") ||
           type == QStringLiteral("pong") ||
           type == QStringLiteral("event.status_snapshot") ||
           type == QStringLiteral("event.binding_changed") ||
           type == QStringLiteral("event.video_state_changed") ||
           type == QStringLiteral("event.control_error") ||
           type == QStringLiteral("externalWindow.attach") ||
           type == QStringLiteral("externalWindow.source_parked") ||
           type == QStringLiteral("externalWindow.source_input") ||
           type == QStringLiteral("externalWindow.tab_capture") ||
           type == QStringLiteral("externalWindow.detach") ||
           type == QStringLiteral("result");
}

bool isDesktopCommandType(const QString& type) {
    return type == QStringLiteral("command.boss_hide") ||
           type == QStringLiteral("command.boss_restore") ||
           type == QStringLiteral("command.finalize_boss_restore") ||
           type == QStringLiteral("command.toggle_playback") ||
           type == QStringLiteral("command.video_control") ||
           type == QStringLiteral("command.show_course_window") ||
           type == QStringLiteral("command.hide_course_window") ||
           type == QStringLiteral("command.return_tab") ||
           type == QStringLiteral("command.query_status");
}

QJsonObject makeMessage(const QString& type, const QJsonObject& payload,
                        const QString& requestId) {
    return {
        {QStringLiteral("protocolVersion"), kVersion},
        {QStringLiteral("type"), type},
        {QStringLiteral("requestId"), requestId.isEmpty()
            ? QUuid::createUuid().toString(QUuid::WithoutBraces) : requestId},
        {QStringLiteral("timestamp"), QDateTime::currentMSecsSinceEpoch()},
        {QStringLiteral("payload"), payload},
    };
}

}  // namespace quizpane::browser::protocol
