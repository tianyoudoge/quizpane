#pragma once

#include <QObject>
#include <QKeySequence>

#if defined(Q_OS_WIN)
#include <QAbstractNativeEventFilter>
#endif

class QSocketNotifier;

namespace quizpane {

// 对三套系统 API 的统一封装：macOS Carbon、Windows RegisterHotKey、UOS X11。
// UI 层只处理 QKeySequence，不需要知道各平台的虚拟键码和事件循环差异。
class GlobalHotkey final : public QObject
#if defined(Q_OS_WIN)
    , public QAbstractNativeEventFilter
#endif
{
    Q_OBJECT

public:
    explicit GlobalHotkey(QObject* parent = nullptr);
    ~GlobalHotkey() override;
    bool registerBossKey(const QKeySequence& sequence,
                         QString* error = nullptr);
    [[nodiscard]] QKeySequence sequence() const;

#if defined(Q_OS_WIN)
    bool nativeEventFilter(const QByteArray& eventType, void* message,
                           qintptr* result) override;
#endif

signals:
    void activated();

private:
    void unregisterBossKey();
    void* nativeHandle_ = nullptr;
    void* nativeHandler_ = nullptr;
    QSocketNotifier* socketNotifier_ = nullptr;
    bool registered_ = false;
    QKeySequence sequence_;
};

}  // namespace quizpane
