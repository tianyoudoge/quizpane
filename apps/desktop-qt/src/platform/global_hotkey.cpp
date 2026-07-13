#include "global_hotkey.hpp"

#include <QApplication>
#include <QMetaObject>
#include <QSocketNotifier>

#if defined(Q_OS_MACOS)
#include <Carbon/Carbon.h>
#elif defined(Q_OS_WIN)
#include <windows.h>
#elif defined(QUIZPANE_HAVE_X11)
#include <X11/Xlib.h>
#include <X11/keysym.h>
#endif

namespace quizpane {
namespace {
constexpr int kHotkeyId = 0x5150;

bool parseHotkey(const QKeySequence& sequence, int* key,
                 Qt::KeyboardModifiers* modifiers, QString* error) {
    // 只开放“修饰键 + 字母/数字”的交集，避免某个平台接受、另一个平台无法表示。
    // 这是一种 portability-first 的校验策略。
    if (sequence.count() != 1) {
        if (error) *error = QStringLiteral("老板键必须是一个按键组合");
        return false;
    }
    const QKeyCombination combination = sequence[0];
    const int parsedKey = combination.key();
    const auto parsedModifiers = combination.keyboardModifiers();
    if (!((parsedKey >= Qt::Key_A && parsedKey <= Qt::Key_Z) ||
          (parsedKey >= Qt::Key_0 && parsedKey <= Qt::Key_9)) ||
        parsedModifiers == Qt::NoModifier) {
        if (error)
            *error = QStringLiteral("请选择带修饰键的字母或数字，例如 Ctrl+Shift+H");
        return false;
    }
    *key = parsedKey;
    *modifiers = parsedModifiers;
    return true;
}

#if defined(Q_OS_MACOS)
UInt32 macKeyCode(int key) {
    switch (key) {
    case Qt::Key_A: return kVK_ANSI_A; case Qt::Key_B: return kVK_ANSI_B;
    case Qt::Key_C: return kVK_ANSI_C; case Qt::Key_D: return kVK_ANSI_D;
    case Qt::Key_E: return kVK_ANSI_E; case Qt::Key_F: return kVK_ANSI_F;
    case Qt::Key_G: return kVK_ANSI_G; case Qt::Key_H: return kVK_ANSI_H;
    case Qt::Key_I: return kVK_ANSI_I; case Qt::Key_J: return kVK_ANSI_J;
    case Qt::Key_K: return kVK_ANSI_K; case Qt::Key_L: return kVK_ANSI_L;
    case Qt::Key_M: return kVK_ANSI_M; case Qt::Key_N: return kVK_ANSI_N;
    case Qt::Key_O: return kVK_ANSI_O; case Qt::Key_P: return kVK_ANSI_P;
    case Qt::Key_Q: return kVK_ANSI_Q; case Qt::Key_R: return kVK_ANSI_R;
    case Qt::Key_S: return kVK_ANSI_S; case Qt::Key_T: return kVK_ANSI_T;
    case Qt::Key_U: return kVK_ANSI_U; case Qt::Key_V: return kVK_ANSI_V;
    case Qt::Key_W: return kVK_ANSI_W; case Qt::Key_X: return kVK_ANSI_X;
    case Qt::Key_Y: return kVK_ANSI_Y; case Qt::Key_Z: return kVK_ANSI_Z;
    case Qt::Key_0: return kVK_ANSI_0; case Qt::Key_1: return kVK_ANSI_1;
    case Qt::Key_2: return kVK_ANSI_2; case Qt::Key_3: return kVK_ANSI_3;
    case Qt::Key_4: return kVK_ANSI_4; case Qt::Key_5: return kVK_ANSI_5;
    case Qt::Key_6: return kVK_ANSI_6; case Qt::Key_7: return kVK_ANSI_7;
    case Qt::Key_8: return kVK_ANSI_8; case Qt::Key_9: return kVK_ANSI_9;
    default: return UINT32_MAX;
    }
}
#endif

#if defined(Q_OS_MACOS)
OSStatus hotkeyHandler(EventHandlerCallRef, EventRef event, void* userData) {
    EventHotKeyID identifier{};
    GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID,
                      nullptr, sizeof(identifier), nullptr, &identifier);
    if (identifier.id != kHotkeyId || !userData) return eventNotHandledErr;
    auto* hotkey = static_cast<GlobalHotkey*>(userData);
    QMetaObject::invokeMethod(hotkey, &GlobalHotkey::activated,
                              Qt::QueuedConnection);
    return noErr;
}
#endif
}  // namespace

GlobalHotkey::GlobalHotkey(QObject* parent) : QObject(parent) {}

GlobalHotkey::~GlobalHotkey() { unregisterBossKey(); }

bool GlobalHotkey::registerBossKey(const QKeySequence& sequence, QString* error) {
    int key = 0;
    Qt::KeyboardModifiers modifiers;
    if (!parseHotkey(sequence, &key, &modifiers, error)) return false;
    // 系统全局热键是进程外共享资源，注册新键前必须释放旧键。UI 层在失败时会
    // 用 previous sequence 回滚，等价于一个很小的补偿事务。
    unregisterBossKey();
#if defined(Q_OS_MACOS)
    EventTypeSpec eventType{kEventClassKeyboard, kEventHotKeyPressed};
    EventHandlerRef handler = nullptr;
    if (InstallApplicationEventHandler(&hotkeyHandler, 1, &eventType, this,
                                       &handler) != noErr) {
        if (error) *error = QStringLiteral("无法安装 macOS 全局热键处理器");
        return false;
    }
    EventHotKeyRef hotkey = nullptr;
    const EventHotKeyID identifier{'QzPn', kHotkeyId};
    UInt32 nativeModifiers = 0;
    if (modifiers.testFlag(Qt::ControlModifier)) nativeModifiers |= controlKey;
    if (modifiers.testFlag(Qt::ShiftModifier)) nativeModifiers |= shiftKey;
    if (modifiers.testFlag(Qt::AltModifier)) nativeModifiers |= optionKey;
    if (modifiers.testFlag(Qt::MetaModifier)) nativeModifiers |= cmdKey;
    if (RegisterEventHotKey(macKeyCode(key), nativeModifiers, identifier,
                            GetApplicationEventTarget(), 0, &hotkey) != noErr) {
        RemoveEventHandler(handler);
        if (error) *error = QStringLiteral("%1 已被其他程序占用")
                                .arg(sequence.toString(QKeySequence::NativeText));
        return false;
    }
    nativeHandle_ = hotkey;
    nativeHandler_ = handler;
    registered_ = true;
    sequence_ = sequence;
    return true;
#elif defined(Q_OS_WIN)
    UINT nativeModifiers = MOD_NOREPEAT;
    if (modifiers.testFlag(Qt::ControlModifier)) nativeModifiers |= MOD_CONTROL;
    if (modifiers.testFlag(Qt::ShiftModifier)) nativeModifiers |= MOD_SHIFT;
    if (modifiers.testFlag(Qt::AltModifier)) nativeModifiers |= MOD_ALT;
    if (modifiers.testFlag(Qt::MetaModifier)) nativeModifiers |= MOD_WIN;
    if (!RegisterHotKey(nullptr, kHotkeyId, nativeModifiers,
                        static_cast<UINT>(key))) {
        if (error) *error = QStringLiteral("%1 已被其他程序占用")
                                .arg(sequence.toString(QKeySequence::NativeText));
        return false;
    }
    qApp->installNativeEventFilter(this);
    registered_ = true;
    sequence_ = sequence;
    return true;
#elif defined(QUIZPANE_HAVE_X11)
    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        if (error) *error = QStringLiteral("当前 UOS 会话不是可用的 X11 会话");
        return false;
    }
    const Window root = DefaultRootWindow(display);
    const KeySym symbol = key >= Qt::Key_A && key <= Qt::Key_Z
        ? XK_A + (key - Qt::Key_A) : XK_0 + (key - Qt::Key_0);
    const int keycode = XKeysymToKeycode(display, symbol);
    unsigned int base = 0;
    if (modifiers.testFlag(Qt::ControlModifier)) base |= ControlMask;
    if (modifiers.testFlag(Qt::ShiftModifier)) base |= ShiftMask;
    if (modifiers.testFlag(Qt::AltModifier)) base |= Mod1Mask;
    if (modifiers.testFlag(Qt::MetaModifier)) base |= Mod4Mask;
    for (unsigned int extra : {
             0u,
             static_cast<unsigned int>(LockMask),
             static_cast<unsigned int>(Mod2Mask),
             static_cast<unsigned int>(LockMask | Mod2Mask)})
        XGrabKey(display, keycode, base | extra, root, True,
                 GrabModeAsync, GrabModeAsync);
    XSync(display, False);
    nativeHandle_ = display;
    socketNotifier_ = new QSocketNotifier(ConnectionNumber(display),
                                           QSocketNotifier::Read, this);
    connect(socketNotifier_, &QSocketNotifier::activated, this, [this] {
        auto* display = static_cast<Display*>(nativeHandle_);
        while (display && XPending(display)) {
            XEvent event{};
            XNextEvent(display, &event);
            if (event.type == KeyPress) emit activated();
        }
    });
    registered_ = true;
    sequence_ = sequence;
    return true;
#else
    if (error) *error = QStringLiteral("当前平台尚不支持系统级老板键");
    return false;
#endif
}

QKeySequence GlobalHotkey::sequence() const { return sequence_; }

void GlobalHotkey::unregisterBossKey() {
    // 析构函数也调用这里，符合 RAII：对象生命周期结束时自动归还 OS 资源。
    if (!registered_) return;
#if defined(Q_OS_MACOS)
    if (nativeHandle_) UnregisterEventHotKey(
        static_cast<EventHotKeyRef>(nativeHandle_));
    if (nativeHandler_) RemoveEventHandler(
        static_cast<EventHandlerRef>(nativeHandler_));
#elif defined(Q_OS_WIN)
    qApp->removeNativeEventFilter(this);
    UnregisterHotKey(nullptr, kHotkeyId);
#elif defined(QUIZPANE_HAVE_X11)
    delete socketNotifier_;
    socketNotifier_ = nullptr;
    if (nativeHandle_) XCloseDisplay(static_cast<Display*>(nativeHandle_));
#endif
    nativeHandle_ = nullptr;
    nativeHandler_ = nullptr;
    registered_ = false;
}

#if defined(Q_OS_WIN)
bool GlobalHotkey::nativeEventFilter(const QByteArray&, void* message,
                                     qintptr*) {
    const auto* msg = static_cast<MSG*>(message);
    if (msg && msg->message == WM_HOTKEY && msg->wParam == kHotkeyId) {
        emit activated();
        return true;
    }
    return false;
}
#endif

}  // namespace quizpane
