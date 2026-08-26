#include "quizpane/diagnostic_logger.hpp"

#if defined(QUIZPANE_DIAGNOSTIC_LOGGING) || defined(QUIZPANE_PROD_DIAGNOSTICS)

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDebug>
#include <QDesktopServices>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTextStream>
#include <QThread>
#include <QUrl>

#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <exception>

#if defined(Q_OS_WIN)
#include <windows.h>
#include <dbghelp.h>
#else
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace quizpane::diagnostic {
namespace {

constexpr qint64 kMaximumLogBytes = 5 * 1024 * 1024;
constexpr int kRetainedLogs = 2;

// 两层构建开关：
// - QUIZPANE_DIAGNOSTIC_LOGGING：DEBUG 语义，info 级全量落盘（现状不变）。
// - QUIZPANE_PROD_DIAGNOSTICS：release 可用的最小集——只落 warning+ 的 Qt 消息
//   和自有 event() 面包屑，崩溃捕获常开，运行时可被用户设置关闭。
// 两者同时打开时以 DEBUG 语义为准（prod 分支全部退化为无条件行为）。
// 这必须是预处理宏：模式差异不仅影响运行时逻辑，还影响 platform-specific
// 符号是否编译。不能以 C++ constexpr 替代后再写进 #if。
#if defined(QUIZPANE_PROD_DIAGNOSTICS) && !defined(QUIZPANE_DIAGNOSTIC_LOGGING)
#define QUIZPANE_DIAGNOSTIC_PROD_MODE 1
#else
#define QUIZPANE_DIAGNOSTIC_PROD_MODE 0
#endif

// 全局单例状态，用 QMutex 保护并发写入（日志可能来自多个线程的 qInfo 调用）。
struct LogState {
    QFile file;
    QMutex mutex;
    QString path;
    QString crashPath;
    QString component;
    QtMessageHandler previousHandler = nullptr;
    bool initialized = false;
};

#if !defined(Q_OS_WIN)
// POSIX 下崩溃时要用信号安全的方式写文件，不能再走 QFile。
// DEBUG 模式沿用 initialize 时预打开的裸 fd；prod 模式在崩溃时现开（open()
// 是 async-signal-safe 的），因为日志开关可能运行时被用户打开。
int crashFileDescriptor = -1;
char crashPathBuffer[4096] = {};
#endif

LogState& state() {
    // 有意不析构：进程退出末期仍可能出现 Qt 消息，避免静态析构顺序导致崩溃。
    static auto* value = new LogState;
    return *value;
}

#if QUIZPANE_DIAGNOSTIC_PROD_MODE
// 生产包运行时开关（QSettings，默认开）。用裸 atomic 缓存一份：崩溃 handler
// 里不能碰 QSettings（非 async-signal-safe），只能读这个标志。
std::atomic<bool> enabledFlag{true};
#endif

// 日志可能包含 Provider 请求头或调试打印，这里做一次正则脱敏，防止
// Authorization/Bearer token、API Key 明文落盘。
QString redact(QString message) {
    static const QRegularExpression bearer(
        QStringLiteral("(?i)(authorization\\s*[:=]\\s*bearer\\s+)[^\\s,;]+"));
    static const QRegularExpression apiKey(
        QStringLiteral("(?i)((?:api[_-]?key|token)\\s*[:=]\\s*)[^\\s,;]+"));
    message.replace(bearer, QStringLiteral("\\1<redacted>"));
    message.replace(apiKey, QStringLiteral("\\1<redacted>"));
    return message;
}

#if QUIZPANE_DIAGNOSTIC_PROD_MODE
// 生产日志随反馈上报离开本机，完整路径里带着用户名，落盘前只保留末两级。
QString maskPaths(QString message) {
    auto shorten = [](const QString& matched, QChar separator) {
        const QStringList parts = matched.split(separator, Qt::SkipEmptyParts);
        if (parts.size() <= 2)
            return matched;
        return QStringLiteral("...") + separator + parts.mid(parts.size() - 2).join(separator);
    };
    static const QRegularExpression unixPath(
        QStringLiteral(R"((?:/Users|/home)/[\w.-]+(?:/[\w.-]+)*)"));
    static const QRegularExpression windowsPath(
        QStringLiteral(R"(([A-Za-z]:\\(?:Users|Public|程序数据|ProgramData)\\[\w.-]+(?:\\[\w.-]+)*))"));
    // Qt 的 replace 没有回调重载，这里按 globalMatch 的结果手动拼接。
    auto apply = [&shorten](QString& text, const QRegularExpression& pattern, QChar separator) {
        QString result;
        qsizetype position = 0;
        auto matcher = pattern.globalMatch(text);
        while (matcher.hasNext()) {
            const auto match = matcher.next();
            result += text.mid(position, match.capturedStart() - position);
            result += shorten(match.captured(0), separator);
            position = match.capturedEnd();
        }
        result += text.mid(position);
        text = result;
    };
    apply(message, unixPath, QChar('/'));
    apply(message, windowsPath, QChar('\\'));
    return message;
}
#endif

QString levelName(QtMsgType type) {
    switch (type) {
    case QtDebugMsg: return QStringLiteral("DEBUG");
    case QtInfoMsg: return QStringLiteral("INFO");
    case QtWarningMsg: return QStringLiteral("WARN");
    case QtCriticalMsg: return QStringLiteral("ERROR");
    case QtFatalMsg: return QStringLiteral("FATAL");
    }
    return QStringLiteral("UNKNOWN");
}

bool isWarningOrWorse(QtMsgType type) {
    // QtMsgType 的枚举值并不按严重程度单调排列：QtInfoMsg 的值高于
    // QtWarningMsg，不能以 type >= QtWarningMsg 判断。
    return type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg;
}

void rotate(const QString& path) {
    if (QFileInfo(path).size() < kMaximumLogBytes)
        return;
    QFile::remove(path + QStringLiteral(".%1").arg(kRetainedLogs));
    for (int index = kRetainedLogs - 1; index >= 1; --index)
        QFile::rename(path + QStringLiteral(".%1").arg(index),
                      path + QStringLiteral(".%1").arg(index + 1));
    QFile::rename(path, path + QStringLiteral(".1"));
}

// 把一行写进日志文件（调用方已持有 mutex）。文件超过上限时先轮转再写。
void appendLine(LogState& log, const QString& level, const QString& content) {
    if (!log.file.isOpen())
        return;
    if (log.file.size() >= kMaximumLogBytes) {
        log.file.close();
        rotate(log.path);
        log.file.setFileName(log.path);
        if (!log.file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
            return;
    }
    QTextStream stream(&log.file);
    stream << QDateTime::currentDateTime().toString(Qt::ISODateWithMs)
           << " pid=" << QCoreApplication::applicationPid()
           << " tid=" << reinterpret_cast<quintptr>(QThread::currentThreadId())
           << ' ' << level << ' ' << content << Qt::endl;
    log.file.flush();
}

// 通过 qInstallMessageHandler 挂载，所有 qInfo/qWarning/
// qCritical 都会先经过这里再转发给 previousHandler（Qt 默认
// 打印到 stderr 的那个 handler），相当于给 Qt 的日志管道加一层
// 落盘 + 脱敏 + 滚动切割。
void messageHandler(QtMsgType type, const QMessageLogContext& context,
                    const QString& message) {
    LogState& log = state();
    {
        QMutexLocker locker(&log.mutex);
#if QUIZPANE_DIAGNOSTIC_PROD_MODE
        // 每次消息都刷新一次开关（QSettings 有内存缓存，成本很低）：
        // 用户在设置里重新打开日志后，下一条 warning 就会自动重开文件。
        enabledFlag = QSettings().value(QStringLiteral("diagnosticsEnabled"),
                                        true).toBool();
        if (log.initialized && enabledFlag && isWarningOrWorse(type)) {
            if (!log.file.isOpen()) {
                log.file.setFileName(log.path);
                (void)log.file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
            }
            const QString safe = maskPaths(redact(message));
            appendLine(log, levelName(type), safe);
        }
#else
        if (log.file.isOpen()) {
            const QString safe = redact(message);
            appendLine(log, levelName(type), safe);
        }
#endif
    }
    if (log.previousHandler)
        log.previousHandler(type, context, message);
    else
        std::fprintf(stderr, "%s\n", message.toLocal8Bit().constData());
}

// Windows/POSIX 的崩溃捕获机制完全不同，因此这里按平台
// 各自实现一套：Windows 用 SEH 的
// SetUnhandledExceptionFilter + MiniDumpWriteDump 生成可以
// 在 WinDbg/Visual Studio 里加载分析的 .dmp 文件；POSIX 没有
// 等价 API，只能装 signal handler，在崩溃信号触发时用
// backtrace() 抓栈帧地址，backtrace_symbols_fd 直接写文件
// 描述符（信号处理函数里不能安全地做内存分配，所以不能用
// QString/QFile，只能用最原始的 write() 系统调用）。
#if defined(Q_OS_WIN)
void writeMiniDump(EXCEPTION_POINTERS* exception) {
#if QUIZPANE_DIAGNOSTIC_PROD_MODE
    if (!enabledFlag.load(std::memory_order_relaxed))
        return;
#endif
    const QString path = state().crashPath;
    HANDLE file = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_WRITE,
                              FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    MINIDUMP_EXCEPTION_INFORMATION information{};
    information.ThreadId = GetCurrentThreadId();
    information.ExceptionPointers = exception;
    information.ClientPointers = FALSE;
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                      static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo |
                                                MiniDumpWithIndirectlyReferencedMemory),
                      exception ? &information : nullptr, nullptr, nullptr);
    CloseHandle(file);
}

LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* exception) {
    writeMiniDump(exception);
    return EXCEPTION_EXECUTE_HANDLER;
}
#else
// 崩溃信号处理函数（signal handler）的黄金法则：只能调用
// async-signal-safe 的函数。write/backtrace_symbols_fd
// 满足这个要求，QFile/QString 不满足。
void writeBacktrace(int signalNumber) {
#if QUIZPANE_DIAGNOSTIC_PROD_MODE
    if (!enabledFlag.load(std::memory_order_relaxed) || crashPathBuffer[0] == '\0')
        return;
    // open() 是 async-signal-safe 的；崩溃时现开，无需 initialize 预开。
    const int fd = ::open(crashPathBuffer, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0)
        return;
    const int descriptor = fd;
#else
    const int descriptor = crashFileDescriptor;
    if (descriptor < 0)
        return;
#endif
    static constexpr char header[] = "\n=== fatal signal backtrace ===\n";
    ::write(descriptor, header, sizeof(header) - 1);
    void* frames[128];
    const int count = ::backtrace(frames, 128);
    ::backtrace_symbols_fd(frames, count, descriptor);
    static constexpr char footer[] = "=== end backtrace ===\n";
    ::write(descriptor, footer, sizeof(footer) - 1);
    ::fsync(descriptor);
#if QUIZPANE_DIAGNOSTIC_PROD_MODE
    ::close(descriptor);
#endif
    ::signal(signalNumber, SIG_DFL);
    ::raise(signalNumber);
}

void fatalSignalHandler(int signalNumber) {
    writeBacktrace(signalNumber);
    ::_exit(128 + signalNumber);
}

// SA_RESETHAND：处理一次后恢复该信号的默认行为，随后
// writeBacktrace 里的 raise(signalNumber) 会让进程按系统
// 默认方式（生成 core dump 等）终止，不会死循环递归进
// 同一个 handler。
void installFatalSignalHandlers() {
    for (const int signalNumber : {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE}) {
        struct sigaction action{};
        action.sa_handler = fatalSignalHandler;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESETHAND;
        sigaction(signalNumber, &action, nullptr);
    }
}
#endif

// 生产包两种崩溃机制的统一点火开关（DEBUG 模式在 initialize 内联安装）。
#if defined(Q_OS_WIN)
void installCrashHandlers() {
    SetUnhandledExceptionFilter(unhandledExceptionFilter);
}
#else
void installCrashHandlers() {
    installFatalSignalHandlers();
}
#endif

QString fieldValue(const QVariant& value) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    if (value.metaType().id() == QMetaType::Bool)
#else
    if (value.type() == QVariant::Bool)
#endif
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    return value.toString().replace(QChar('\n'), QChar(' ')).left(500);
}

} // namespace

bool isDiagnosticsEnabled() {
#if QUIZPANE_DIAGNOSTIC_PROD_MODE
    return QSettings().value(QStringLiteral("diagnosticsEnabled"), true).toBool();
#else
    return true;
#endif
}

void setDiagnosticsEnabled(bool enabled) {
#if QUIZPANE_DIAGNOSTIC_PROD_MODE
    QSettings().setValue(QStringLiteral("diagnosticsEnabled"), enabled);
    enabledFlag.store(enabled);
#else
    Q_UNUSED(enabled);
#endif
}

bool initialize(const QString& component) {
    LogState& log = state();
    QMutexLocker locker(&log.mutex);
    if (log.initialized)
        return true;
    const QString directory = QDir(QStandardPaths::writableLocation(
        QStandardPaths::GenericDataLocation)).filePath(QStringLiteral("QuizPane/logs"));
    if (!QDir().mkpath(directory))
        return false;
    log.component = component;
    log.path = QDir(directory).filePath(component + QStringLiteral("-debug.log"));
    log.crashPath = QDir(directory).filePath(component +
#if defined(Q_OS_WIN)
        QStringLiteral("-crash.dmp"));
#else
        QStringLiteral("-crash.log"));
#endif
    log.initialized = true;
    log.previousHandler = qInstallMessageHandler(messageHandler);
#if QUIZPANE_DIAGNOSTIC_PROD_MODE
    // 生产包：即使用户已关闭日志，也要装好 handler——崩溃捕获不受
    // 「日志开关」之外的因素影响，且用户随时可以在设置里重新打开。
    enabledFlag = isDiagnosticsEnabled();
#if !defined(Q_OS_WIN)
    // POSIX 崩溃处理器只能操作原始路径缓冲区；Windows 的 minidump
    // 路径直接在 writeMiniDump() 中通过 Qt 状态取得。
    std::snprintf(crashPathBuffer, sizeof(crashPathBuffer), "%s",
                  log.crashPath.toUtf8().constData());
#endif
    if (enabledFlag.load(std::memory_order_relaxed)) {
        rotate(log.path);
        log.file.setFileName(log.path);
        (void)log.file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    }
    installCrashHandlers();
#else
    rotate(log.path);
    log.file.setFileName(log.path);
    if (!log.file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return false;
#if defined(Q_OS_WIN)
    SetUnhandledExceptionFilter(unhandledExceptionFilter);
#else
    rotate(log.crashPath);
    const QByteArray crashPath = QFile::encodeName(log.crashPath);
    crashFileDescriptor = ::open(crashPath.constData(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    installFatalSignalHandlers();
#endif
#endif
    // std::set_terminate 兜底 signal handler 之外的路径：
    // 未捕获 C++ 异常、noexcept 函数里抛异常等场景走的是
    // std::terminate 而非 fatal signal，两套机制覆盖面不同，
    // 必须都装上才能保证崩溃总能留下痕迹。
    std::set_terminate([] {
        qCritical("std::terminate invoked; process will abort");
#if defined(Q_OS_WIN)
        writeMiniDump(nullptr);
#else
        writeBacktrace(SIGABRT);
#endif
        std::abort();
    });
    locker.unlock();
    event(QStringLiteral("session"), QStringLiteral("start"),
          {{QStringLiteral("app"), QCoreApplication::applicationName()},
           {QStringLiteral("version"), QCoreApplication::applicationVersion()},
           {QStringLiteral("qt"), QString::fromLatin1(qVersion())},
           {QStringLiteral("os"), QSysInfo::prettyProductName()},
           {QStringLiteral("cpu"), QSysInfo::currentCpuArchitecture()},
           {QStringLiteral("log"), log.path},
           {QStringLiteral("crashArtifact"), log.crashPath}});
    return true;
}

void payload(const QString& area, const QString& name, const QString& label,
             const QString& content, qsizetype maximumCharacters) {
#ifdef QUIZPANE_VERBOSE_DIAGNOSTICS
    if (!state().initialized)
        return;
    const qsizetype limit = qBound(qsizetype{0}, maximumCharacters,
                                   qsizetype{256 * 1024});
    const bool truncated = content.size() > limit;
    const QString captured = content.left(limit);
    qInfo().noquote()
        << QStringLiteral("[%1] %2 label=%3 characters=%4 captured=%5 truncated=%6\n"
                          "--- BEGIN %3 ---\n%7\n--- END %3 ---")
               .arg(area, name, label)
               .arg(content.size())
               .arg(captured.size())
               .arg(truncated ? QStringLiteral("true") : QStringLiteral("false"), captured);
#else
    Q_UNUSED(area);
    Q_UNUSED(name);
    Q_UNUSED(label);
    Q_UNUSED(content);
    Q_UNUSED(maximumCharacters);
#endif
}

void event(const QString& area, const QString& name, const QVariantMap& fields) {
    LogState& log = state();
    if (!log.initialized)
        return;
    QStringList parts;
    for (auto it = fields.cbegin(); it != fields.cend(); ++it)
        parts.append(QStringLiteral("%1=%2").arg(it.key(), fieldValue(it.value())));
    const QString line = QStringLiteral("[%1] %2%3")
                             .arg(area, name, parts.isEmpty()
                                 ? QString{} : QStringLiteral(" ") + parts.join(QChar(' ')));
#if QUIZPANE_DIAGNOSTIC_PROD_MODE
    // 生产包里 event() 是我们自己的低频结构化面包屑，不走 messageHandler
    // 的 warning+ 过滤（否则全被丢掉），直接落盘；同样先脱敏。
    enabledFlag = isDiagnosticsEnabled();
    if (!enabledFlag.load(std::memory_order_relaxed))
        return;
    QMutexLocker locker(&log.mutex);
    if (!log.file.isOpen()) {
        log.file.setFileName(log.path);
        (void)log.file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    }
    appendLine(log, QStringLiteral("EVENT"), maskPaths(redact(line)));
#else
    qInfo().noquote() << line;
#endif
}

void shutdown() {
    LogState& log = state();
    if (!log.initialized)
        return;
    event(QStringLiteral("session"), QStringLiteral("end"),
          {{QStringLiteral("exit"), QStringLiteral("clean")}});
    QMutexLocker locker(&log.mutex);
    log.file.flush();
#if QUIZPANE_DIAGNOSTIC_PROD_MODE
    // 生产包恢复 Qt 原生的消息处理器，退出后不再有任何 Qt 消息回调开销。
    log.file.close();
    if (log.previousHandler)
        qInstallMessageHandler(log.previousHandler);
    log.previousHandler = nullptr;
    log.initialized = false;
#endif
}

QString logFilePath() {
    return state().path;
}

QString crashArtifactPath() {
    return state().crashPath;
}

bool openLogFile() {
    if (state().path.isEmpty())
        return false;
    return QDesktopServices::openUrl(QUrl::fromLocalFile(state().path));
}

} // namespace quizpane::diagnostic

#else

namespace quizpane::diagnostic {
bool initialize(const QString&) { return false; }
void event(const QString&, const QString&, const QVariantMap&) {}
void payload(const QString&, const QString&, const QString&, const QString&, qsizetype) {}
void shutdown() {}
bool isDiagnosticsEnabled() { return true; }
void setDiagnosticsEnabled(bool) {}
QString logFilePath() { return {}; }
QString crashArtifactPath() { return {}; }
bool openLogFile() { return false; }
} // namespace quizpane::diagnostic

#endif
