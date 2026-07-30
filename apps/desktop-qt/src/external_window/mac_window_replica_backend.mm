#include "mac_window_replica_controller.hpp"

#import <CoreGraphics/CoreGraphics.h>

namespace quizpane::external_window {

MacWindowReplicaBackend::MacWindowReplicaBackend(QObject* parent) : QObject(parent) {
    // __bridge_retained：把 ARC 管理的 Objective-C 对象转成裸指针，同时让
    // ARC 放弃这块内存的自动释放权，改由 C++ 侧手动持有（析构里的
    // CFBridgingRelease 是唯一归还所有权的地方，二者必须成对出现）。
    controller_ = (__bridge_retained void*)[[QPMacReplicaController alloc] initWithOwner:this];
}

MacWindowReplicaBackend::~MacWindowReplicaBackend() {
    detach();
    if (controller_ != nullptr) {
        [(__bridge QPMacReplicaController*)controller_ invalidateOwner];
        CFBridgingRelease(controller_);
        controller_ = nullptr;
    }
}

void MacWindowReplicaBackend::attach(const AttachRequest& request) {
    [(__bridge QPMacReplicaController*)controller_ startWithRequest:request];
}

void MacWindowReplicaBackend::detach() {
    [(__bridge QPMacReplicaController*)controller_ detach];
}

void MacWindowReplicaBackend::setVisible(bool visible) {
    [(__bridge QPMacReplicaController*)controller_ setVisible:visible];
}

void MacWindowReplicaBackend::prepareForRestore() {
    [(__bridge QPMacReplicaController*)controller_ prepareForRestore];
}

bool MacWindowReplicaBackend::requestScreenCapturePermission() {
    if (!@available(macOS 13.0, *)) return false;
    return CGPreflightScreenCaptureAccess() || CGRequestScreenCaptureAccess();
}

void MacWindowReplicaBackend::reportAttachFinished(const AttachResult& result) {
    emit attachFinished(result);
}

void MacWindowReplicaBackend::reportRestoreFrameReady() {
    emit restoreFrameReady();
}

void MacWindowReplicaBackend::reportVideoControl(
    const QString& action, double normalizedPosition) {
    emit videoControlRequested(action, normalizedPosition);
}

}  // namespace quizpane::external_window
