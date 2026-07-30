#pragma once

#include "external_window_types.hpp"

#include <QObject>

namespace quizpane::external_window {

// 对外统一入口：调用方（做题窗口）只认这一个类，不关心当前
// 跑在哪个平台后端上。跨平台差异全部封装在 Private 里，用
// 编译期 #if 选择成员和实现（PIMPL 惯用法，好处是这个头文件
// 不用 #include windows.h/AppKit.h 之类的平台专属头）。
class ExternalWindowManager final : public QObject {
    Q_OBJECT

public:
    explicit ExternalWindowManager(QObject* parent = nullptr);
    ~ExternalWindowManager() override;

    void attach(const AttachRequest& request);
    void detach();
    void setVisible(bool visible);
    void prepareForRestore();
    bool requestScreenCapturePermission();
    [[nodiscard]] State state() const;

signals:
    void attachFinished(const quizpane::external_window::AttachResult& result);
    void restoreFrameReady();
    void stateChanged(quizpane::external_window::State state, const QString& detail);
    void videoControlRequested(const QString& action, double normalizedPosition);

private:
    void tryAttachWindows();

    class Private;
    Private* d_ = nullptr;
};

}  // namespace quizpane::external_window
