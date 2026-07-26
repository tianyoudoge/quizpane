#include "external_window_manager.hpp"

#if defined(Q_OS_MACOS)
#include "mac_window_replica_backend.hpp"
#elif defined(Q_OS_WIN)
#include "windows_window_topmost_backend.hpp"
#endif

#include <memory>

namespace quizpane::external_window {

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
    const AttachResult result = d_->windowsBackend->attach(request);
    d_->state = result.success ? State::Active : State::Failed;
    emit stateChanged(d_->state, result.success
        ? QStringLiteral("网页小窗已置顶显示") : result.error);
    emit attachFinished(result);
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

}  // namespace quizpane::external_window
