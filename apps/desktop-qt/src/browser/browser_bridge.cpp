#include "browser_bridge.hpp"

#include "browser_protocol.hpp"
#include "../external_window/external_window_manager.hpp"

#include <QCoreApplication>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>
#include <QRegularExpression>
#include <QWebSocket>

namespace quizpane::browser {
namespace {

void setError(QString* error, const QString& text) {
    if (error) *error = text;
}

}  // namespace

BrowserBridge::BrowserBridge(QObject* parent)
    : QObject(parent),
      server_(QStringLiteral("QuizPane browser bridge"),
              QWebSocketServer::NonSecureMode, this) {
    connect(&server_, &QWebSocketServer::newConnection,
            this, &BrowserBridge::acceptConnection);
    externalWindowManager_ = new external_window::ExternalWindowManager(this);
    connect(externalWindowManager_, &external_window::ExternalWindowManager::attachFinished,
            this, &BrowserBridge::publishExternalWindowAttached);
    connect(externalWindowManager_, &external_window::ExternalWindowManager::videoControlRequested,
            this, [this](const QString& action, double normalizedPosition) {
                QJsonObject payload{{QStringLiteral("action"), action}};
                if (normalizedPosition >= 0.0)
                    payload.insert(QStringLiteral("position"), normalizedPosition);
                sendCommand(QStringLiteral("command.video_control"), payload);
            });
    connect(externalWindowManager_, &external_window::ExternalWindowManager::restoreFrameReady,
            this, [this] {
#if defined(Q_OS_MACOS)
                qInfo() << "[QuizPane][BossRestore] mirror frame presented";
                bossRestoreFrameReady_ = true;
                finishBossRestoreIfReady();
#endif
            });
}

bool BrowserBridge::start(QString* error) {
    if (server_.isListening()) return true;
    // LocalHost 明确选择 IPv4 的 127.0.0.1，扩展不会尝试局域网或公网地址。
    if (!server_.listen(QHostAddress::LocalHost, kBridgePort)) {
        setError(error, QStringLiteral("浏览器联动端口 %1 无法监听：%2")
            .arg(kBridgePort).arg(server_.errorString()));
        return false;
    }
    return true;
}

void BrowserBridge::stop() {
    if (client_) client_->close();
    server_.close();
}

BrowserStatus BrowserBridge::status() const { return status_; }

void BrowserBridge::requestBossHide() {
    qInfo() << "[QuizPane][BossRestore] hide requested";
    pendingBossRestoreRequestId_.clear();
    bossRestoreCommandCompleted_ = false;
    bossRestoreFrameReady_ = false;
    externalWindowManager_->setVisible(false);
    sendCommand(QStringLiteral("command.boss_hide"));
}
void BrowserBridge::requestBossRestore() {
#if defined(Q_OS_MACOS)
    // 必须先记录最小化状态下的帧序号，再恢复 Chromium。否则解除最小化时的
    // 第一张关键帧可能早于命令回执到达，后续静止画面便不会再产生新帧。
    bossRestoreCommandCompleted_ = false;
    bossRestoreFrameReady_ = false;
    externalWindowManager_->prepareForRestore();
    pendingBossRestoreRequestId_ = sendCommand(QStringLiteral("command.boss_restore"));
    qInfo() << "[QuizPane][BossRestore] restore requested"
            << pendingBossRestoreRequestId_;
#else
    externalWindowManager_->setVisible(true);
    sendCommand(QStringLiteral("command.boss_restore"));
#endif
}
void BrowserBridge::requestTogglePlayback() {
    sendCommand(QStringLiteral("command.toggle_playback"));
}
void BrowserBridge::requestShowCourseWindow() {
    externalWindowManager_->setVisible(true);
    sendCommand(QStringLiteral("command.show_course_window"));
}
void BrowserBridge::requestHideCourseWindow() {
    externalWindowManager_->setVisible(false);
    sendCommand(QStringLiteral("command.hide_course_window"));
}
void BrowserBridge::requestReturnTab() {
    externalWindowManager_->detach();
    BrowserStatus next = status_;
    next.externalWindowActive = false;
    next.externalWindowError.clear();
    setStatus(next);
    sendCommand(QStringLiteral("command.return_tab"));
}
void BrowserBridge::requestStatus() { sendCommand(QStringLiteral("command.query_status")); }
bool BrowserBridge::requestScreenCapturePermission() {
    return externalWindowManager_->requestScreenCapturePermission();
}

void BrowserBridge::acceptConnection() {
    while (QWebSocket* socket = server_.nextPendingConnection()) {
        if (socket->requestUrl().path() != QString::fromLatin1(kBridgePath) || client_) {
            socket->close(QWebSocketProtocol::CloseCodePolicyViolated,
                          QStringLiteral("QuizPane browser bridge only permits one client"));
            socket->deleteLater();
            continue;
        }
        client_ = socket;
        socket->setMaxAllowedIncomingFrameSize(protocol::kMaxMessageBytes);
        socket->setMaxAllowedIncomingMessageSize(protocol::kMaxMessageBytes);
        helloReceived_ = false;
        connect(socket, &QWebSocket::textMessageReceived, this,
                [this, socket](const QString& text) { handleMessage(socket, text); });
        connect(socket, &QWebSocket::disconnected, this,
                [this, socket] { disconnectClient(socket); });
    }
}

void BrowserBridge::handleMessage(QWebSocket* socket, const QString& text) {
    if (socket != client_) return;
    QJsonObject message;
    QString error;
    if (!protocol::parseMessage(text, &message, &error)) {
        socket->close(QWebSocketProtocol::CloseCodePolicyViolated, error);
        return;
    }
    const QString type = message.value(QStringLiteral("type")).toString();
    if (!protocol::isExtensionMessageType(type)) {
        socket->close(QWebSocketProtocol::CloseCodePolicyViolated,
                      QStringLiteral("不允许的浏览器消息"));
        return;
    }
    const QJsonObject payload = message.value(QStringLiteral("payload")).toObject();
    if (type == QStringLiteral("ping")) {
        socket->sendTextMessage(QString::fromUtf8(QJsonDocument(protocol::makeMessage(
            QStringLiteral("pong"), {}, message.value(QStringLiteral("requestId")).toString()))
            .toJson(QJsonDocument::Compact)));
        return;
    }
    if (type == QStringLiteral("hello")) {
        if (payload.value(QStringLiteral("client")).toString() !=
            QStringLiteral("quizpane-browser-extension")) {
            socket->close(QWebSocketProtocol::CloseCodePolicyViolated,
                          QStringLiteral("不支持的浏览器客户端"));
            return;
        }
        helloReceived_ = true;
        socket->sendTextMessage(QString::fromUtf8(QJsonDocument(protocol::makeMessage(
            QStringLiteral("hello_ack"),
            {{QStringLiteral("success"), true},
             {QStringLiteral("appVersion"), QCoreApplication::applicationVersion()},
             {QStringLiteral("heartbeatSeconds"), 20}},
            message.value(QStringLiteral("requestId")).toString())).toJson(QJsonDocument::Compact)));
        BrowserStatus next = status_;
        next.connection = ConnectionState::Connected;
        next.browserName = payload.value(QStringLiteral("browser")).toString();
        setStatus(next);
        return;
    }
    if (!helloReceived_) {
        socket->close(QWebSocketProtocol::CloseCodePolicyViolated,
                      QStringLiteral("必须先完成 hello"));
        return;
    }
    if (type == QStringLiteral("event.status_snapshot") ||
        type == QStringLiteral("event.binding_changed") ||
        type == QStringLiteral("event.video_state_changed")) {
        BrowserStatus next = status_;
        next.courseBound = payload.value(QStringLiteral("bound")).toBool();
        next.courseWindowVisible = payload.value(QStringLiteral("courseWindowVisible")).toBool();
        next.videoDetected = payload.value(QStringLiteral("videoDetected")).toBool();
        next.videoPlaying = payload.value(QStringLiteral("videoState")).toString() ==
                            QStringLiteral("playing");
        next.videoCurrentTimeSeconds = qMax<qint64>(-1,
            static_cast<qint64>(payload.value(QStringLiteral("videoCurrentTimeSeconds")).toDouble(-1)));
        next.videoDurationSeconds = qMax<qint64>(-1,
            static_cast<qint64>(payload.value(QStringLiteral("videoDurationSeconds")).toDouble(-1)));
        if (const QString title = payload.value(QStringLiteral("courseTitle")).toString();
            !title.isEmpty()) next.courseTitle = title;
        setStatus(next);
        return;
    }
    if (type == QStringLiteral("externalWindow.attach")) {
        const QString sessionId = payload.value(QStringLiteral("sessionId")).toString();
        const QString bindingToken = payload.value(QStringLiteral("bindingToken")).toString();
        const QJsonObject bounds = payload.value(QStringLiteral("bounds")).toObject();
        static const QRegularExpression tokenPattern(
            QStringLiteral(R"(^__QUIZPANE_WINDOW_[0-9a-fA-F-]{16,}__$)"));
        if (sessionId.isEmpty() || !tokenPattern.match(bindingToken).hasMatch() ||
            bounds.isEmpty()) {
            external_window::AttachResult result;
            result.sessionId = sessionId;
            result.error = QStringLiteral("网页小窗绑定请求无效");
            publishExternalWindowAttached(result);
            return;
        }
        external_window::AttachRequest request;
        request.sessionId = sessionId;
        request.bindingToken = bindingToken;
        request.chromeWindowId = static_cast<qint64>(payload.value(QStringLiteral("chromeWindowId")).toDouble(-1));
        request.browserReportedBounds = QRect(
            bounds.value(QStringLiteral("left")).toInt(), bounds.value(QStringLiteral("top")).toInt(),
            bounds.value(QStringLiteral("width")).toInt(), bounds.value(QStringLiteral("height")).toInt());
        request.preferredFps = qBound(15, payload.value(QStringLiteral("preferredFps")).toInt(30), 60);
        if (request.browserReportedBounds.width() <= 0 || request.browserReportedBounds.height() <= 0) {
            external_window::AttachResult result;
            result.sessionId = sessionId;
            result.error = QStringLiteral("网页小窗尺寸无效");
            publishExternalWindowAttached(result);
            return;
        }
        externalWindowManager_->attach(request);
        return;
    }
    if (type == QStringLiteral("externalWindow.detach")) {
        externalWindowManager_->detach();
        BrowserStatus next = status_;
        next.externalWindowActive = false;
        next.externalWindowError.clear();
        setStatus(next);
        return;
    }
    if (type == QStringLiteral("result")) {
        const QString requestId = message.value(QStringLiteral("requestId")).toString();
#if defined(Q_OS_MACOS)
        if (!pendingBossRestoreRequestId_.isEmpty() &&
            requestId == pendingBossRestoreRequestId_) {
            bossRestoreCommandCompleted_ = true;
            qInfo() << "[QuizPane][BossRestore] browser restore completed"
                    << payload.value(QStringLiteral("success")).toBool();
            finishBossRestoreIfReady();
        }
#endif
        emit commandCompleted(requestId,
                              payload.value(QStringLiteral("success")).toBool(),
                              payload.value(QStringLiteral("error")).toString());
    }
}

void BrowserBridge::publishExternalWindowAttached(const external_window::AttachResult& result) {
    BrowserStatus next = status_;
    next.externalWindowActive = result.success;
    next.externalWindowError = result.success ? QString{} : result.error;
    setStatus(next);
    if (!client_ || !helloReceived_) return;
    const auto& caps = result.capabilities;
    const QJsonObject payload{
        {QStringLiteral("sessionId"), result.sessionId},
        {QStringLiteral("success"), result.success},
        {QStringLiteral("backend"), external_window::backendName(result.backend)},
        {QStringLiteral("error"), result.error},
        {QStringLiteral("capabilities"), QJsonObject{
            {QStringLiteral("interactive"), caps.interactive},
            {QStringLiteral("capture"), caps.supportsCapture},
            {QStringLiteral("opacity"), caps.supportsOpacity},
            {QStringLiteral("fitMode"), caps.supportsFitMode},
            {QStringLiteral("requiresScreenCapturePermission"), caps.requiresScreenCapturePermission},
        }},
    };
    client_->sendTextMessage(QString::fromUtf8(QJsonDocument(protocol::makeMessage(
        QStringLiteral("externalWindow.attached"), payload)).toJson(QJsonDocument::Compact)));
}

QString BrowserBridge::sendCommand(const QString& type, const QJsonObject& payload) {
    if (!client_ || !helloReceived_ || !protocol::isDesktopCommandType(type)) return {};
    const QJsonObject message = protocol::makeMessage(type, payload);
    client_->sendTextMessage(QString::fromUtf8(
        QJsonDocument(message).toJson(QJsonDocument::Compact)));
    return message.value(QStringLiteral("requestId")).toString();
}

void BrowserBridge::finishBossRestoreIfReady() {
#if defined(Q_OS_MACOS)
    if (pendingBossRestoreRequestId_.isEmpty() ||
        !bossRestoreCommandCompleted_ || !bossRestoreFrameReady_) return;
    qInfo() << "[QuizPane][BossRestore] handshake complete; keeping source normal";
    pendingBossRestoreRequestId_.clear();
    bossRestoreCommandCompleted_ = false;
    bossRestoreFrameReady_ = false;
    sendCommand(QStringLiteral("command.finalize_boss_restore"));
#endif
}

void BrowserBridge::setStatus(const BrowserStatus& status) {
    if (status_.connection == status.connection && status_.browserName == status.browserName &&
        status_.courseTitle == status.courseTitle && status_.courseBound == status.courseBound &&
        status_.courseWindowVisible == status.courseWindowVisible &&
        status_.externalWindowActive == status.externalWindowActive &&
        status_.externalWindowError == status.externalWindowError &&
        status_.videoDetected == status.videoDetected && status_.videoPlaying == status.videoPlaying &&
        status_.videoCurrentTimeSeconds == status.videoCurrentTimeSeconds &&
        status_.videoDurationSeconds == status.videoDurationSeconds) return;
    status_ = status;
    emit statusChanged(status_);
}

void BrowserBridge::disconnectClient(QWebSocket* socket) {
    if (socket != client_) return;
    client_->deleteLater();
    client_ = nullptr;
    helloReceived_ = false;
    pendingBossRestoreRequestId_.clear();
    bossRestoreCommandCompleted_ = false;
    bossRestoreFrameReady_ = false;
    externalWindowManager_->detach();
    BrowserStatus next;
    setStatus(next);
}

}  // namespace quizpane::browser
