#pragma once

#include "external_window_types.hpp"

#include <QObject>

namespace quizpane::external_window {

class ExternalWindowManager final : public QObject {
    Q_OBJECT

public:
    explicit ExternalWindowManager(QObject* parent = nullptr);
    ~ExternalWindowManager() override;

    void attach(const AttachRequest& request);
    void detach();
    void setPinned(bool pinned);
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
