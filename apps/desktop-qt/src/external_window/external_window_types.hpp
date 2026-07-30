#pragma once

#include <QRect>
#include <QString>
#include <QMetaType>

namespace quizpane::external_window {

// 同一套接口下四种平台实现：Windows 直接控制原生窗口置顶
// （不复制画面），macOS 用 ScreenCaptureKit 把源窗口画面镜像
// 渲染到自己的窗口，Linux 两种合成协议各一套（目前未启用）。
enum class BackendType {
    Unsupported,
    WindowsNativeTopmost,
    MacCapturedReplica,
    LinuxX11NativeTopmost,
    LinuxWaylandPortalReplica,
};

// 单向流转的会话状态机：Idle -> ResolvingTarget（定位源窗口）
// -> Attached/Starting -> Active；Recovering 用于源窗口暂时
// 丢失后的自动重连；Failed/Stopped 为终态。
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
