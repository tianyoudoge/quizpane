#include "mac_window_replica_controller.hpp"

#include <QMetaObject>

#include <algorithm>
#include <cmath>

@interface QPMirrorView ()
@property(nonatomic, weak) QPMacReplicaController* mirrorController;
@end

@implementation QPMirrorView

- (instancetype)initWithFrame:(NSRect)frame
                       device:(id<MTLDevice>)device
                   controller:(QPMacReplicaController*)controller {
    self = [super initWithFrame:frame device:device];
    if (self) _mirrorController = controller;
    return self;
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (void)mouseDown:(NSEvent*)event {
    [self.window makeFirstResponder:self];
    [self.mirrorController handleMirrorPointer:
        [self convertPoint:event.locationInWindow fromView:nil]
                                             phase:0
                                          viewSize:self.bounds.size];
}

- (void)mouseDragged:(NSEvent*)event {
    [self.mirrorController handleMirrorPointer:
        [self convertPoint:event.locationInWindow fromView:nil]
                                             phase:1
                                          viewSize:self.bounds.size];
}

- (void)mouseUp:(NSEvent*)event {
    [self.mirrorController handleMirrorPointer:
        [self convertPoint:event.locationInWindow fromView:nil]
                                             phase:2
                                          viewSize:self.bounds.size];
}

@end

@implementation QPMacReplicaController (InputForwarding)

- (void)emitVideoControl:(const QString&)action position:(double)position {
    auto* owner = _owner;
    if (owner == nullptr) return;
    QMetaObject::invokeMethod(owner, [owner, action, position] {
        owner->reportVideoControl(action, position);
    }, Qt::QueuedConnection);
}

- (void)handleMirrorPointer:(NSPoint)point phase:(NSInteger)phase viewSize:(NSSize)viewSize {
    if (viewSize.width <= 0 || viewSize.height <= 0) return;
    const double position = std::clamp(point.x / viewSize.width, 0.0, 1.0);
    if (phase == 0) {
        _pointerDown = point;
        _seeking = point.y <= 56.0;
        if (_seeking) {
            _lastSeekEmissionTime = CFAbsoluteTimeGetCurrent();
            [self emitVideoControl:QStringLiteral("seek") position:position];
        }
        return;
    }
    if (_seeking) {
        const CFTimeInterval now = CFAbsoluteTimeGetCurrent();
        if (phase == 2 || now - _lastSeekEmissionTime >= (1.0 / 30.0)) {
            _lastSeekEmissionTime = now;
            [self emitVideoControl:QStringLiteral("seek") position:position];
        }
        if (phase == 2) _seeking = NO;
        return;
    }
    if (phase == 2) {
        const double distance = std::hypot(
            point.x - _pointerDown.x, point.y - _pointerDown.y);
        if (distance <= 8.0) {
            [self emitVideoControl:QStringLiteral("toggle") position:-1.0];
        }
    }
}

@end
