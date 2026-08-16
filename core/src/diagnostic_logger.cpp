#include "quizpane/diagnostic_logger.hpp"

#ifdef QUIZPANE_DIAGNOSTIC_LOGGING

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
#include <QStandardPaths>
#include <QSysInfo>
#include <QTextStream>
#include <QThread>
#include <QUrl>

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

// POSIX 下崩溃时要用信号安全的方式写文件，不能再走 QFile；这里保留一个
// 裸文件描述符，signal handler 内直接调用 write()/backtrace_symbols_fd()。
#if !defined(Q_OS_WIN)
int crashFileDescriptor = -1;
#endif

LogState& state() {
    // 有意不析构：进程退出末期仍可能出现 Qt 消息，避免静态析构顺序导致崩溃。
    static auto* value = new LogState;
    return *value;
}

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

void rotate(const QString& path) {
    if (QFileInfo(path).size() < kMaximumLogBytes)
        return;
    QFile::remove(path + QStringLiteral(".%1").arg(kRetainedLogs));
    for (int index = kRetainedLogs - 1; index >= 1; --index)
        QFile::rename(path + QStringLiteral(".%1").arg(index),
                      path + QStringLiteral(".%1").arg(index + 1));
    QFile::rename(path, path + QStringLiteral(".1"));
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
        if (log.file.isOpen()) {
            if (log.file.size() >= kMaximumLogBytes) {
                log.file.close();
                rotate(log.path);
                log.file.setFileName(log.path);
                if (!log.file.open(QIODevice::WriteOnly | QIODevice::Append |
                                   QIODevice::Text))
                    return;
            }
            QTextStream stream(&log.file);
            stream << QDateTime::currentDateTime().toString(Qt::ISODateWithMs)
                   << " pid=" << QCoreApplication::applicationPid()
                   << " tid=" << reinterpret_cast<quintptr>(QThread::currentThreadId())
                   << ' ' << levelName(type) << ' ';
            if (context.category && *context.category)
                stream << '[' << context.category << "] ";
            stream << redact(message);
            if (context.file && *context.file)
                stream << " (" << QFileInfo(QString::fromUtf8(context.file)).fileName()
                       << ':' << context.line << ')';
            stream << Qt::endl;
            log.file.flush();
        }
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
    if (crashFileDescriptor < 0)
        return;
    static constexpr char header[] = "\n=== fatal signal backtrace ===\n";
    ::write(crashFileDescriptor, header, sizeof(header) - 1);
    void* frames[128];
    const int count = ::backtrace(frames, 128);
    ::backtrace_symbols_fd(frames, count, crashFileDescriptor);
    static constexpr char footer[] = "=== end backtrace ===\n";
    ::write(crashFileDescriptor, footer, sizeof(footer) - 1);
    ::fsync(crashFileDescriptor);
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
    rotate(log.path);
    log.file.setFileName(log.path);
    if (!log.file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return false;
    log.previousHandler = qInstallMessageHandler(messageHandler);
    log.initialized = true;
#if defined(Q_OS_WIN)
    SetUnhandledExceptionFilter(unhandledExceptionFilter);
#else
    rotate(log.crashPath);
    const QByteArray crashPath = QFile::encodeName(log.crashPath);
    crashFileDescriptor = ::open(crashPath.constData(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    installFatalSignalHandlers();
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
    if (!state().initialized)
        return;
    QStringList parts;
    for (auto it = fields.cbegin(); it != fields.cend(); ++it)
        parts.append(QStringLiteral("%1=%2").arg(it.key(), fieldValue(it.value())));
    qInfo().noquote() << QStringLiteral("[%1] %2%3")
                            .arg(area, name, parts.isEmpty()
                                ? QString{} : QStringLiteral(" ") + parts.join(QChar(' ')));
}

void shutdown() {
    LogState& log = state();
    if (!log.initialized)
        return;
    event(QStringLiteral("session"), QStringLiteral("end"),
          {{QStringLiteral("exit"), QStringLiteral("clean")}});
    QMutexLocker locker(&log.mutex);
    log.file.flush();
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
QString logFilePath() { return {}; }
QString crashArtifactPath() { return {}; }
bool openLogFile() { return false; }
} // namespace quizpane::diagnostic

#endif
