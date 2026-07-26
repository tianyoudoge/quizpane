#pragma once

#include "../external_window/external_window_types.hpp"

#include <QJsonObject>
#include <QObject>
#include <QWebSocketServer>

class QWebSocket;
namespace quizpane::external_window { class ExternalWindowManager; }

namespace quizpane::browser {

inline constexpr quint16 kBridgePort = 49752;
inline constexpr auto kBridgePath = "/quizpane-browser/v1";

enum class ConnectionState { Disconnected, Connected };

struct BrowserStatus {
    ConnectionState connection = ConnectionState::Disconnected;
    QString browserName;
    QString courseTitle;
    bool courseBound = false;
    bool courseWindowVisible = false;
    bool externalWindowActive = false;
    QString externalWindowError;
    bool videoDetected = false;
    bool videoPlaying = false;
    qint64 videoCurrentTimeSeconds = -1;
    qint64 videoDurationSeconds = -1;
};

// 本机桥接只监听 IPv4 loopback。它不接收任何文件、命令或网页数据，扩展和
// 桌面端之间只交换少量播放器状态与白名单控制命令。
class BrowserBridge final : public QObject {
    Q_OBJECT

public:
    explicit BrowserBridge(QObject* parent = nullptr);

    bool start(QString* error = nullptr);
    void stop();
    [[nodiscard]] BrowserStatus status() const;

    void requestBossHide();
    void requestBossRestore();
    void requestTogglePlayback();
    void requestShowCourseWindow();
    void requestHideCourseWindow();
    void requestReturnTab();
    void requestStatus();
    bool requestScreenCapturePermission();

signals:
    void statusChanged(const quizpane::browser::BrowserStatus& status);
    void commandCompleted(const QString& requestId, bool success, const QString& error);

private:
    void acceptConnection();
    void handleMessage(QWebSocket* socket, const QString& text);
    QString sendCommand(const QString& type, const QJsonObject& payload = {});
    void setStatus(const BrowserStatus& status);
    void disconnectClient(QWebSocket* socket);
    void publishExternalWindowAttached(const quizpane::external_window::AttachResult& result);
    void finishBossRestoreIfReady();

    QWebSocketServer server_;
    QWebSocket* client_ = nullptr;
    BrowserStatus status_;
    quizpane::external_window::ExternalWindowManager* externalWindowManager_ = nullptr;
    bool helloReceived_ = false;
    QString pendingBossRestoreRequestId_;
    bool bossRestoreCommandCompleted_ = false;
    bool bossRestoreFrameReady_ = false;
};

}  // namespace quizpane::browser
