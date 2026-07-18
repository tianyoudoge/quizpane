#include "quizpane/running_app_handoff.hpp"

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>

namespace quizpane {

QString runningAppControlServerName() {
    const QString overridden = qEnvironmentVariable("QUIZPANE_CONTROL_SERVER_NAME").trimmed();
    if (!overridden.isEmpty())
        return overridden;
    return QStringLiteral("org.quizpane.control.v1");
}

bool handoffProviderToRunningApp(const QString& providerEntryPath, QString* error) {
    const QString path = QFileInfo(providerEntryPath).absoluteFilePath();
    if (!QFileInfo(path).isFile()) {
        if (error) *error = QStringLiteral("新题库入口文件不存在");
        return false;
    }
    QLocalSocket socket;
    socket.connectToServer(runningAppControlServerName(), QIODevice::ReadWrite);
    if (!socket.waitForConnected(350)) {
        if (error) *error = QStringLiteral("未检测到正在运行的小窗刷题");
        return false;
    }
    const QByteArray request = QJsonDocument(QJsonObject{
        {QStringLiteral("command"), QStringLiteral("load-provider")},
        {QStringLiteral("path"), path}}).toJson(QJsonDocument::Compact) + '\n';
    if (socket.write(request) != request.size() || !socket.waitForBytesWritten(500)) {
        if (error) *error = QStringLiteral("无法向小窗刷题发送新题库");
        return false;
    }
    if (!socket.waitForReadyRead(800)) {
        if (error) *error = QStringLiteral("小窗刷题未确认接收新题库");
        return false;
    }
    const QByteArray response = socket.readLine();
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(response, &parseError);
    if (!document.isObject() || !document.object().value(QStringLiteral("ok")).toBool()) {
        if (error) *error = document.isObject()
            ? document.object().value(QStringLiteral("error")).toString(
                QStringLiteral("小窗刷题拒绝接收新题库"))
            : QStringLiteral("小窗刷题返回了无效响应");
        return false;
    }
    return true;
}

} // namespace quizpane
