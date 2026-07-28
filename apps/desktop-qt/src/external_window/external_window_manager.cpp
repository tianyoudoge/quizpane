#include "external_window_manager.hpp"

#include "quizpane/diagnostic_logger.hpp"

#if defined(Q_OS_MACOS)
#include "mac_window_replica_backend.hpp"
#elif defined(Q_OS_WIN)
#include "windows_window_topmost_backend.hpp"
#endif

#include <QElapsedTimer>
#include <QTimer>

#include <memory>

namespace quizpane::external_window {
namespace {

constexpr int kWindowsAttachRetryIntervalMs = 50;
constexpr int kWindowsAttachTimeoutMs = 2000;

}

QString backendName(BackendType type) {
    switch (type) {
    case BackendType::WindowsNativeTopmost: return QStringLiteral("windows-native-topmost");
    case BackendType::MacCapturedReplica: return QStringLiteral("macos-captured-replica");
    case BackendType::LinuxX11NativeTopmost: return QStringLiteral("linux-x11-native-topmost");
    case BackendType::LinuxWaylandPortalReplica: return QStringLiteral("linux-wayland-portal-replica");
    case BackendType::Unsupported: return QStringLiteral("unsupported");
    }
    return QStringLiteral("unsupported");
}

class ExternalWindowManager::Private {
public:
    State state = State::Idle;
#if defined(Q_OS_MACOS)
    std::unique_ptr<MacWindowReplicaBackend> macBackend;
#elif defined(Q_OS_WIN)
    std::unique_ptr<WindowsWindowTopmostBackend> windowsBackend;
    QTimer windowsAttachRetryTimer;
    QElapsedTimer windowsAttachElapsed;
    AttachRequest pendingWindowsAttach;
    bool windowsAttachPending = false;
    int windowsAttachAttempt = 0;
#endif
};

ExternalWindowManager::ExternalWindowManager(QObject* parent)
    : QObject(parent), d_(new Private) {
#if defined(Q_OS_MACOS)
    d_->macBackend = std::make_unique<MacWindowReplicaBackend>();
    connect(d_->macBackend.get(), &MacWindowReplicaBackend::attachFinished, this,
            [this](const AttachResult& result) {
                d_->state = result.success ? State::Active : State::Failed;
                emit stateChanged(d_->state, result.success
                    ? QStringLiteral("网页小窗已置顶显示") : result.error);
                emit attachFinished(result);
            });
    connect(d_->macBackend.get(), &MacWindowReplicaBackend::videoControlRequested, this,
            &ExternalWindowManager::videoControlRequested);
    connect(d_->macBackend.get(), &MacWindowReplicaBackend::restoreFrameReady, this,
            &ExternalWindowManager::restoreFrameReady);
#elif defined(Q_OS_WIN)
    d_->windowsBackend = std::make_unique<WindowsWindowTopmostBackend>();
    d_->windowsAttachRetryTimer.setSingleShot(true);
    connect(&d_->windowsAttachRetryTimer, &QTimer::timeout, this,
            &ExternalWindowManager::tryAttachWindows);
#endif
}

ExternalWindowManager::~ExternalWindowManager() {
    detach();
    delete d_;
}

void ExternalWindowManager::attach(const AttachRequest& request) {
    detach();
    d_->state = State::ResolvingTarget;
    emit stateChanged(d_->state, QStringLiteral("正在定位课程网页窗口…"));
#if defined(Q_OS_MACOS)
    d_->macBackend->attach(request);
#elif defined(Q_OS_WIN)
    d_->pendingWindowsAttach = request;
    d_->windowsAttachPending = true;
    d_->windowsAttachAttempt = 0;
    d_->windowsAttachElapsed.start();
    tryAttachWindows();
#else
    d_->state = State::Failed;
    AttachResult result;
    result.sessionId = request.sessionId;
    result.error = QStringLiteral("当前平台的网页小窗置顶后端尚未启用");
    emit stateChanged(d_->state, result.error);
    emit attachFinished(result);
#endif
}

void ExternalWindowManager::detach() {
#if defined(Q_OS_MACOS)
    if (d_->macBackend) d_->macBackend->detach();
#elif defined(Q_OS_WIN)
    d_->windowsAttachRetryTimer.stop();
    d_->windowsAttachPending = false;
    if (d_->windowsBackend) d_->windowsBackend->detach();
#endif
    if (d_->state != State::Idle) {
        d_->state = State::Stopped;
        emit stateChanged(d_->state, QStringLiteral("网页小窗已关闭"));
    }
}

void ExternalWindowManager::setPinned(bool pinned) {
#if defined(Q_OS_MACOS)
    if (d_->macBackend) d_->macBackend->setPinned(pinned);
#elif defined(Q_OS_WIN)
    if (d_->windowsBackend) d_->windowsBackend->setPinned(pinned);
#else
    Q_UNUSED(pinned)
#endif
}

void ExternalWindowManager::setVisible(bool visible) {
#if defined(Q_OS_MACOS)
    if (d_->macBackend) d_->macBackend->setVisible(visible);
#elif defined(Q_OS_WIN)
    if (d_->windowsBackend) d_->windowsBackend->setVisible(visible);
#else
    Q_UNUSED(visible)
#endif
}

void ExternalWindowManager::prepareForRestore() {
#if defined(Q_OS_MACOS)
    if (d_->macBackend) d_->macBackend->prepareForRestore();
#elif defined(Q_OS_WIN)
    if (d_->windowsBackend) d_->windowsBackend->setVisible(true);
#endif
}

bool ExternalWindowManager::requestScreenCapturePermission() {
#if defined(Q_OS_MACOS)
    return d_->macBackend && d_->macBackend->requestScreenCapturePermission();
#else
    return true;
#endif
}

State ExternalWindowManager::state() const { return d_->state; }

void ExternalWindowManager::tryAttachWindows() {
#if defined(Q_OS_WIN)
    if (!d_->windowsAttachPending || !d_->windowsBackend) return;

    ++d_->windowsAttachAttempt;
    const AttachResult result = d_->windowsBackend->attach(d_->pendingWindowsAttach);
    if (result.success) {
        diagnostic::event(QStringLiteral("external-window"),
                          QStringLiteral("windows-attach-retry-finished"),
                          {{QStringLiteral("attempt"), d_->windowsAttachAttempt},
                           {QStringLiteral("elapsedMs"), d_->windowsAttachElapsed.elapsed()},
                           {QStringLiteral("success"), true}});
        d_->windowsAttachPending = false;
        d_->state = State::Active;
        emit stateChanged(d_->state, QStringLiteral("网页小窗已置顶显示"));
        emit attachFinished(result);
        return;
    }

    const bool targetNotReady =
        result.error == QStringLiteral("没有找到已绑定的 Chrome 或 Edge 课程小窗");
    const qint64 elapsedMs = d_->windowsAttachElapsed.elapsed();
    if (targetNotReady && elapsedMs < kWindowsAttachTimeoutMs) {
        diagnostic::event(QStringLiteral("external-window"),
                          QStringLiteral("windows-attach-retry-scheduled"),
                          {{QStringLiteral("attempt"), d_->windowsAttachAttempt},
                           {QStringLiteral("elapsedMs"), elapsedMs},
                           {QStringLiteral("delayMs"), kWindowsAttachRetryIntervalMs}});
        d_->windowsAttachRetryTimer.start(kWindowsAttachRetryIntervalMs);
        return;
    }

    diagnostic::event(QStringLiteral("external-window"),
                      QStringLiteral("windows-attach-retry-finished"),
                      {{QStringLiteral("attempt"), d_->windowsAttachAttempt},
                       {QStringLiteral("elapsedMs"), elapsedMs},
                       {QStringLiteral("success"), false},
                       {QStringLiteral("targetNotReady"), targetNotReady}});
    d_->windowsAttachPending = false;
    d_->state = State::Failed;
    emit stateChanged(d_->state, result.error);
    emit attachFinished(result);
#endif
}

}  // namespace quizpane::external_window
