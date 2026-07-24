#include "quizpane/running_app_handoff.hpp"

#include <QCoreApplication>
#include <QDebug>
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
        if (quizpane::handoffProviderToRunningApp(arguments.at(2), &error)) return 0;
        qCritical().noquote() << "Handoff client failed:" << error;
        return 1;
    }

    QTemporaryDir directory;
    if (!directory.isValid()) {
        qCritical() << "Unable to create temporary directory";
        return 2;
    }
    const QString entryPath = directory.filePath(QStringLiteral("bank.json"));
    QFile entry(entryPath);
    if (!entry.open(QIODevice::WriteOnly) || entry.write("{}") != 2) {
        qCritical().noquote() << "Unable to create temporary provider entry:" << entry.errorString();
        return 3;
    }
    entry.close();

    const QString serverName = QStringLiteral("org.quizpane.handoff-test.%1")
        .arg(QCoreApplication::applicationPid());
    qputenv("QUIZPANE_CONTROL_SERVER_NAME", serverName.toUtf8());
    QLocalServer::removeServer(serverName);
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!server.listen(serverName)) {
        qCritical().noquote() << "Unable to listen for handoff:" << server.errorString();
        return 4;
    }

    bool received = false;
    QObject::connect(&server, &QLocalServer::newConnection, &server, [&] {
        while (QLocalSocket* socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QLocalSocket::readyRead, socket, [&, socket] {
                QByteArray buffered = socket->property("handoff-test-buffer").toByteArray();
                buffered += socket->readAll();
                const int newline = buffered.indexOf('\n');
                if (newline < 0) {
                    socket->setProperty("handoff-test-buffer", buffered);
                    return;
                }
                const QJsonDocument request = QJsonDocument::fromJson(buffered.left(newline));
                const QJsonObject object = request.object();
                received = request.isObject() &&
                    object.value(QStringLiteral("command")).toString() == QStringLiteral("load-provider") &&
                    object.value(QStringLiteral("path")).toString() == entryPath;
                if (!received) qCritical().noquote() << "Unexpected handoff request:" << buffered.left(newline);
                socket->write(QJsonDocument(QJsonObject{{QStringLiteral("ok"), received}})
                    .toJson(QJsonDocument::Compact) + '\n');
                socket->flush();
                // 由客户端在读完确认后关闭。这里立即关闭在 Windows named pipe
                // 上会偶发地让客户端先观察到 RemoteClosed。
                QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            });
        }
    });

    QProcess child;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&child, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                     &loop, &QEventLoop::quit);
    child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--client"), entryPath});
    if (!child.waitForStarted(1000)) {
        qCritical().noquote() << "Unable to start handoff client:" << child.errorString();
        return 5;
    }
    timeout.start(3000);
    loop.exec();
    if (!received) qCritical() << "Handoff server did not receive a valid request";
    if (child.exitStatus() != QProcess::NormalExit)
        qCritical().noquote() << "Handoff client crashed:" << child.errorString();
    if (child.exitCode() != 0) {
        qCritical() << "Handoff client exited with" << child.exitCode();
        const QByteArray stderrOutput = child.readAllStandardError().trimmed();
        if (!stderrOutput.isEmpty()) qCritical().noquote() << "Handoff client stderr:" << stderrOutput;
    }
    return received && child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0 ? 0 : 6;
}
