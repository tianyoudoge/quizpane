#include "mac_window_replica_controller.hpp"
#include "quizpane/diagnostic_logger.hpp"

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

// 镜像面板只显示画面，不接收 Chromium 的真实 DOM 事件，因此这里把鼠标
// 交互重新解释成两种语义再转发给 Qt 侧处理：按在底部进度条区域
// （y <= 56pt）算作拖动进度条（seek，按 30fps 节流上报避免刷屏）；
// 其余区域按下-抬起位移小于 8pt 算作一次点击（toggle 播放/暂停），
// 位移更大则视为普通拖拽，不触发任何操作。
- (void)emitVideoControl:(const QString&)action position:(double)position {
    quizpane::diagnostic::event(QStringLiteral("mac-mirror-input"),
                      QStringLiteral("control-emitted"),
                      {{QStringLiteral("sessionId"), _request.sessionId},
                       {QStringLiteral("action"), action},
                       {QStringLiteral("position"), position}});
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
        quizpane::diagnostic::event(QStringLiteral("mac-mirror-input"),
                          QStringLiteral("pointer-down"),
                          {{QStringLiteral("sessionId"), _request.sessionId},
                           {QStringLiteral("x"), point.x},
                           {QStringLiteral("y"), point.y},
                           {QStringLiteral("position"), position},
                           {QStringLiteral("viewWidth"), viewSize.width},
                           {QStringLiteral("viewHeight"), viewSize.height},
                           {QStringLiteral("seeking"), _seeking}});
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
            quizpane::diagnostic::event(QStringLiteral("mac-mirror-input"),
                              phase == 2 ? QStringLiteral("seek-finished")
                                         : QStringLiteral("seek-dragged"),
                              {{QStringLiteral("sessionId"), _request.sessionId},
                               {QStringLiteral("x"), point.x},
                               {QStringLiteral("y"), point.y},
                               {QStringLiteral("position"), position}});
            [self emitVideoControl:QStringLiteral("seek") position:position];
        }
        if (phase == 2) _seeking = NO;
        return;
    }
    if (phase == 2) {
        const double distance = std::hypot(
            point.x - _pointerDown.x, point.y - _pointerDown.y);
        if (distance <= 8.0) {
            quizpane::diagnostic::event(QStringLiteral("mac-mirror-input"),
                              QStringLiteral("pointer-click"),
                              {{QStringLiteral("sessionId"), _request.sessionId},
                               {QStringLiteral("x"), point.x},
                               {QStringLiteral("y"), point.y},
                               {QStringLiteral("distance"), distance}});
            [self emitVideoControl:QStringLiteral("toggle") position:-1.0];
        } else {
            quizpane::diagnostic::event(QStringLiteral("mac-mirror-input"),
                              QStringLiteral("pointer-drag-ignored"),
                              {{QStringLiteral("sessionId"), _request.sessionId},
                               {QStringLiteral("x"), point.x},
                               {QStringLiteral("y"), point.y},
                               {QStringLiteral("distance"), distance}});
        }
    }
}

@end
