#pragma once

#include "external_window_types.hpp"

#include <QObject>

namespace quizpane::external_window {

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
