#pragma once

#include "external_window_types.hpp"

#include <QObject>

namespace quizpane::external_window {

// Qt/C++ 与 Objective-C++（QPMacReplicaController，实现在
// mac_window_replica_capture.mm 等 .mm 文件）之间的桥接层。这个
// 头文件本身是纯 C++，不出现任何 AppKit/ObjC 类型，controller_
// 用 void* 存放桥接后的 Objective-C 对象指针（.mm 里 __bridge
// 转换），这样引用这个类的 .cpp 文件不需要能编译 Objective-C++。
class MacWindowReplicaBackend final : public QObject {
    Q_OBJECT

public:
    explicit MacWindowReplicaBackend(QObject* parent = nullptr);
    ~MacWindowReplicaBackend() override;

    void attach(const AttachRequest& request);
    void detach();
    void setVisible(bool visible);
    void prepareForRestore();
    bool requestScreenCapturePermission();

    // 供 Objective-C++ 的异步 ScreenCaptureKit 回调切回 Qt 线程。
    void reportAttachFinished(const AttachResult& result);
    void reportRestoreFrameReady();
    void reportVideoControl(const QString& action, double normalizedPosition);

signals:
    void attachFinished(const quizpane::external_window::AttachResult& result);
    void restoreFrameReady();
    void videoControlRequested(const QString& action, double normalizedPosition);

private:
    void* controller_ = nullptr;
};

}  // namespace quizpane::external_window
