#pragma once

#include <QRect>
#include <QString>
#include <QMetaType>

namespace quizpane::external_window {

enum class BackendType {
    Unsupported,
    WindowsNativeTopmost,
    MacCapturedReplica,
    LinuxX11NativeTopmost,
    LinuxWaylandPortalReplica,
};

enum class State { Idle, ResolvingTarget, Attached, Starting, Active, Recovering, Failed, Stopped };

enum class AttachError {
    None,
    TargetNotFound,
    TopmostRejected,
    Unsupported,
    InvalidRequest,
    CaptureFailed,
};

struct Capabilities {
    bool interactive = false;
    bool supportsCapture = false;
    bool supportsOpacity = false;
    bool supportsFitMode = false;
    bool requiresScreenCapturePermission = false;
};

struct AttachRequest {
    QString sessionId;
    QString bindingToken;
    qint64 chromeWindowId = -1;
    QRect browserReportedBounds;
    int preferredFps = 30;
};

struct AttachResult {
    QString sessionId;
    bool success = false;
    BackendType backend = BackendType::Unsupported;
    Capabilities capabilities;
    AttachError errorCode = AttachError::None;
    QString error;
};

QString backendName(BackendType type);

}  // namespace quizpane::external_window

Q_DECLARE_METATYPE(quizpane::external_window::AttachResult)
Q_DECLARE_METATYPE(quizpane::external_window::State)
