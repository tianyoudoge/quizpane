#include "window_pinning.hpp"

#import <AppKit/AppKit.h>

#include <QWidget>

namespace quizpane::platform {

void applyNativeWindowPin(QWidget* widget, bool pinned) {
    if (!widget) return;
    // QWidget::winId() 在 macOS 对应 NSView。直接设置 NSWindow level，确保应用
    // 失去键盘焦点后仍位于普通应用窗口之上，而不仅仅是在本应用内部 raise。
    NSView* view = reinterpret_cast<NSView*>(widget->winId());
    NSWindow* window = view.window;
    if (!window) return;

    window.hidesOnDeactivate = NO;
    window.level = pinned ? NSFloatingWindowLevel : NSNormalWindowLevel;

    NSWindowCollectionBehavior behavior = window.collectionBehavior;
    if (pinned) {
        // Qt 的浮动窗口可能默认带 MoveToActiveSpace；AppKit 禁止它与
        // CanJoinAllSpaces 同时存在，必须先清除，否则 setCollectionBehavior
        // 会抛出 NSInternalInconsistencyException。
        behavior &= ~NSWindowCollectionBehaviorMoveToActiveSpace;
        behavior |= NSWindowCollectionBehaviorCanJoinAllSpaces;
        behavior |= NSWindowCollectionBehaviorFullScreenAuxiliary;
    } else {
        behavior &= ~NSWindowCollectionBehaviorCanJoinAllSpaces;
        behavior &= ~NSWindowCollectionBehaviorFullScreenAuxiliary;
    }
    window.collectionBehavior = behavior;
}

}  // namespace quizpane::platform
