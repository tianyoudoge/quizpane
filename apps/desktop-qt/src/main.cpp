#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QNetworkProxyFactory>
#include <QSet>
#include <QTimer>

#include <functional>
#include <exception>
#include <utility>

#include "platform/file_association.hpp"
#include "quizpane/diagnostic_logger.hpp"
#include "quizpane/running_app_handoff.hpp"
#include "ui/main_window.hpp"

namespace {

class QuizPaneApplication final : public QApplication {
public:
    using QApplication::QApplication;

    void setFileOpenHandler(std::function<void(const QString&)> handler) {
        fileOpenHandler_ = std::move(handler);
        for (const QString& path : std::as_const(pendingFiles_)) fileOpenHandler_(path);
        pendingFiles_.clear();
    }

protected:
    bool notify(QObject* receiver, QEvent* event) override {
        try {
            return QApplication::notify(receiver, event);
        } catch (const std::exception& error) {
            quizpane::diagnostic::event(QStringLiteral("qt"),
                QStringLiteral("unhandled-event-exception"),
                {{QStringLiteral("receiver"), receiver ? receiver->metaObject()->className() : "null"},
                 {QStringLiteral("eventType"), event ? static_cast<int>(event->type()) : -1},
                 {QStringLiteral("error"), QString::fromLocal8Bit(error.what())}});
            throw;
        } catch (...) {
            quizpane::diagnostic::event(QStringLiteral("qt"),
                QStringLiteral("unhandled-event-exception"),
                {{QStringLiteral("receiver"), receiver ? receiver->metaObject()->className() : "null"},
                 {QStringLiteral("eventType"), event ? static_cast<int>(event->type()) : -1},
                 {QStringLiteral("error"), QStringLiteral("unknown")}});
            throw;
        }
    }

    bool event(QEvent* event) override {
        if (event->type() == QEvent::FileOpen) {
            const QString path = static_cast<QFileOpenEvent*>(event)->file();
            if (!path.endsWith(QStringLiteral(".quizpane-provider"), Qt::CaseInsensitive))
                return QApplication::event(event);
            if (fileOpenHandler_) fileOpenHandler_(path);
            else pendingFiles_.append(path);
            return true;
        }
        return QApplication::event(event);
    }

private:
    std::function<void(const QString&)> fileOpenHandler_;
    QStringList pendingFiles_;
};

}  // namespace

namespace {

void sendControlReply(QLocalSocket* socket, bool ok, const QString& error = {}) {
    if (!socket) return;
    QJsonObject response{{QStringLiteral("ok"), ok}};
    if (!error.isEmpty()) response.insert(QStringLiteral("error"), error);
    socket->write(QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n');
    socket->flush();
    // 不能在 write 后立即断开：Windows named pipe 可能先把 RemoteClosed 交给
    // 客户端，导致确认 JSON 尚未来得及读取。客户端收到一行确认后会自行关闭；
    // 超时兜底则避免恶意或异常客户端长期占用 socket。
    QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
    QTimer::singleShot(5000, socket, [socket] {
        if (socket->state() != QLocalSocket::UnconnectedState) socket->disconnectFromServer();
    });
}

void installProviderHandoffServer(QLocalServer* server, quizpane::MainWindow* window) {
    if (!server || !window) return;
    const QString name = quizpane::runningAppControlServerName();
    server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!server->listen(name)) {
        // Unix 域 socket 可能因异常退出留下文件；只在确实无法连接到现有实例时
        // 清理它，绝不把另一个仍活着的小窗刷题踢下线。
        QLocalSocket probe;
        probe.connectToServer(name);
        if (!probe.waitForConnected(100)) {
            QLocalServer::removeServer(name);
            server->listen(name);
        }
    }
    if (!server->isListening()) {
        quizpane::diagnostic::event(QStringLiteral("handoff"), QStringLiteral("server-unavailable"),
            {{QStringLiteral("name"), name}, {QStringLiteral("error"), server->errorString()}});
        return;
    }
    QObject::connect(server, &QLocalServer::newConnection, server, [server, window] {
        while (QLocalSocket* socket = server->nextPendingConnection()) {
            QObject::connect(socket, &QLocalSocket::readyRead, socket, [socket, window] {
                QByteArray buffered = socket->property("quizpane-control-buffer").toByteArray();
                buffered += socket->readAll();
                if (buffered.size() > 16 * 1024) {
                    sendControlReply(socket, false, QStringLiteral("控制消息过大"));
                    return;
                }
                const int newline = buffered.indexOf('\n');
                if (newline < 0) {
                    socket->setProperty("quizpane-control-buffer", buffered);
                    return;
                }
                QJsonParseError parseError;
                const QJsonDocument request = QJsonDocument::fromJson(buffered.left(newline), &parseError);
                if (!request.isObject() || request.object().value(QStringLiteral("command")).toString() !=
                        QStringLiteral("load-provider")) {
                    sendControlReply(socket, false, QStringLiteral("不支持的控制命令"));
                    return;
                }
                const QString path = QFileInfo(request.object().value(QStringLiteral("path")).toString())
                    .absoluteFilePath();
                if (!QFileInfo(path).isFile()) {
                    sendControlReply(socket, false, QStringLiteral("题库入口文件不存在"));
                    return;
                }
                window->show();
                window->raise();
                window->activateWindow();
                if (!window->loadProvider(path)) {
                    sendControlReply(socket, false, QStringLiteral("小窗刷题无法加载这份题库"));
                    return;
                }
                quizpane::diagnostic::event(QStringLiteral("handoff"), QStringLiteral("provider-accepted"),
                    {{QStringLiteral("file"), QFileInfo(path).fileName()}});
                sendControlReply(socket, true);
            });
        }
    });
    quizpane::diagnostic::event(QStringLiteral("handoff"), QStringLiteral("server-ready"),
        {{QStringLiteral("name"), name}});
}

} // namespace

int main(int argc, char* argv[]) {
    // QApplication 是 Qt Widgets 的进程级运行时，角色接近浏览器的 window +
    // event loop。所有控件必须在它创建之后、app.exec() 之前构造。
    QuizPaneApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("小窗刷题"));
    QApplication::setApplicationDisplayName(QStringLiteral("小窗刷题"));
    QApplication::setApplicationVersion(QStringLiteral(QUIZPANE_VERSION));
    QApplication::setOrganizationName("QuizPane Project");
    QApplication::setDesktopFileName(QStringLiteral("org.quizpane.app"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/app-icon.png")));
    quizpane::diagnostic::initialize(QStringLiteral("quizpane"));
    quizpane::diagnostic::event(QStringLiteral("app"), QStringLiteral("initialized"),
        {{QStringLiteral("arguments"), argc}});
    // Host 显式初始化 QtNetwork：一方面让 Provider 继承系统代理设置，另一方面
    // 保证 macdeployqt/windeployqt 在没有内置在线题库时也会打包网络运行库。
    QNetworkProxyFactory::setUseSystemConfiguration(true);
    QApplication::setQuitOnLastWindowClosed(false);
    quizpane::platform::registerProviderFileAssociation();

    // 命令行入口主要服务开发调试和“双击安装包”场景；普通用户通过界面菜单操作。
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("小窗刷题桌面客户端"));
    parser.addHelpOption();
    QCommandLineOption providerOption(
        {"p", "provider"}, QStringLiteral("加载题库扩展动态库"), "path");
    parser.addOption(providerOption);
    parser.addPositionalArgument(
        QStringLiteral("package"),
        QStringLiteral("要导入的题库安装包"),
        QStringLiteral("[package]"));
    parser.process(app);

    quizpane::MainWindow window;
    QSet<QString> openedPackages;
    const auto openPackage = [&window, &openedPackages](const QString& path) {
        const QString normalized = QFileInfo(path).absoluteFilePath();
        if (openedPackages.contains(normalized)) {
            quizpane::diagnostic::event(QStringLiteral("package"),
                QStringLiteral("duplicate-open-ignored"),
                {{QStringLiteral("file"), QFileInfo(normalized).fileName()}});
            return;
        }
        openedPackages.insert(normalized);
        quizpane::diagnostic::event(QStringLiteral("package"), QStringLiteral("open"),
            {{QStringLiteral("file"), QFileInfo(normalized).fileName()}});
        window.show();
        window.raise();
        window.activateWindow();
        window.installProviderPackage(normalized);
    };
    app.setFileOpenHandler(openPackage);
    QLocalServer handoffServer;
    installProviderHandoffServer(&handoffServer, &window);
    window.show();
    if (parser.isSet(providerOption)) {
        quizpane::diagnostic::event(QStringLiteral("startup"),
            QStringLiteral("explicit-provider"),
            {{QStringLiteral("file"), QFileInfo(parser.value(providerOption)).fileName()}});
        window.loadProvider(parser.value(providerOption));
    } else if (!parser.positionalArguments().isEmpty()) {
        openPackage(parser.positionalArguments().first());
    } else {
        quizpane::diagnostic::event(QStringLiteral("startup"),
                                    QStringLiteral("load-last-provider"));
        window.loadLastProvider();
    }
    // exec() 进入 UI 事件循环，直到托盘菜单选择“退出程序”才返回。
    const int exitCode = app.exec();
    quizpane::diagnostic::event(QStringLiteral("app"), QStringLiteral("event-loop-returned"),
        {{QStringLiteral("code"), exitCode}});
    quizpane::diagnostic::shutdown();
    return exitCode;
}
