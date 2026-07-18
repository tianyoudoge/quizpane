#include "quizpane/running_app_handoff.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList arguments = app.arguments();
    if (arguments.size() == 3 && arguments.at(1) == QStringLiteral("--client")) {
        QString error;
        return quizpane::handoffProviderToRunningApp(arguments.at(2), &error) ? 0 : 1;
    }

    QTemporaryDir directory;
    if (!directory.isValid()) return 2;
    const QString entryPath = directory.filePath(QStringLiteral("bank.json"));
    QFile entry(entryPath);
    if (!entry.open(QIODevice::WriteOnly) || entry.write("{}") != 2) return 3;
    entry.close();

    const QString serverName = QStringLiteral("org.quizpane.handoff-test.%1")
        .arg(QCoreApplication::applicationPid());
    qputenv("QUIZPANE_CONTROL_SERVER_NAME", serverName.toUtf8());
    QLocalServer::removeServer(serverName);
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!server.listen(serverName)) return 4;

    bool received = false;
    QObject::connect(&server, &QLocalServer::newConnection, &server, [&] {
        QLocalSocket* socket = server.nextPendingConnection();
        if (!socket) return;
        QObject::connect(socket, &QLocalSocket::readyRead, socket, [&, socket] {
            const QJsonDocument request = QJsonDocument::fromJson(socket->readLine());
            const QJsonObject object = request.object();
            received = request.isObject() &&
                object.value(QStringLiteral("command")).toString() == QStringLiteral("load-provider") &&
                object.value(QStringLiteral("path")).toString() == entryPath;
            socket->write(QJsonDocument(QJsonObject{{QStringLiteral("ok"), received}})
                .toJson(QJsonDocument::Compact) + '\n');
            socket->flush();
            socket->disconnectFromServer();
            socket->deleteLater();
        });
    });

    QProcess child;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&child, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                     &loop, &QEventLoop::quit);
    child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--client"), entryPath});
    timeout.start(3000);
    loop.exec();
    return received && child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0 ? 0 : 5;
}
