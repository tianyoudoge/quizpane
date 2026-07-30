#include "../apps/desktop-qt/src/browser/browser_protocol.hpp"

#include <QJsonDocument>

using quizpane::browser::protocol::isDesktopCommandType;
using quizpane::browser::protocol::isExtensionMessageType;
using quizpane::browser::protocol::makeMessage;
using quizpane::browser::protocol::parseMessage;

int main() {
    QJsonObject parsed;
    QString error;
    const QJsonObject message = makeMessage(QStringLiteral("hello"),
        {{QStringLiteral("client"), QStringLiteral("quizpane-browser-extension")}},
        QStringLiteral("test-request"));
    const QString valid = QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact));
    if (!parseMessage(valid, &parsed, &error)) return 1;
    if (parsed.value(QStringLiteral("requestId")).toString() != QStringLiteral("test-request")) return 2;
    if (parseMessage(QStringLiteral("[]"), &parsed, &error)) return 3;
    if (parseMessage(QStringLiteral(R"({"protocolVersion":2,"type":"hello"})"),
                     &parsed, &error)) return 4;
    if (parseMessage(QStringLiteral(R"({"protocolVersion":1,"type":"hello","payload":[]})"),
                     &parsed, &error)) return 5;
    if (!isExtensionMessageType(QStringLiteral("event.status_snapshot")) ||
        !isExtensionMessageType(QStringLiteral("externalWindow.attach")) ||
        !isExtensionMessageType(QStringLiteral("externalWindow.source_parked")) ||
        !isExtensionMessageType(QStringLiteral("externalWindow.source_input")) ||
        !isExtensionMessageType(QStringLiteral("externalWindow.tab_capture")) ||
        !isExtensionMessageType(QStringLiteral("externalWindow.detach")) ||
        isExtensionMessageType(QStringLiteral("command.boss_hide"))) return 6;
    if (!isDesktopCommandType(QStringLiteral("command.boss_restore")) ||
        !isDesktopCommandType(QStringLiteral("command.finalize_boss_restore")) ||
        !isDesktopCommandType(QStringLiteral("command.hide_course_window")) ||
        !isDesktopCommandType(QStringLiteral("command.return_tab")) ||
        isDesktopCommandType(QStringLiteral("open-a-shell"))) return 7;
    return 0;
}
