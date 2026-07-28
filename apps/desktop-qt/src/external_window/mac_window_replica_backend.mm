#include "mac_window_replica_controller.hpp"

#import <CoreGraphics/CoreGraphics.h>

namespace quizpane::external_window {

MacWindowReplicaBackend::MacWindowReplicaBackend(QObject* parent) : QObject(parent) {
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
