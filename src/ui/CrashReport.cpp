#include "core/Brand.h"
#include "ui/CrashReport.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dbghelp.h>
#include <crtdbg.h>
#else
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace crash {

namespace {

// The last lines logged (qDebug, qWarning...), for the report. Written by the
// log hook, read only when crashing.
constexpr int kLines = 64, kLineLen = 240;
char g_log[kLines][kLineLen];
std::atomic<int> g_next{0};
QtMessageHandler g_previous = nullptr;

// Made at install(): a crash handler must not allocate or call Qt.
char g_header[512];
#ifdef Q_OS_WIN
wchar_t g_base[MAX_PATH]; // "<folder>\crash-"
#else
char g_base[1024];
#endif
std::atomic<bool> g_writing{false};

char g_text[24 * 1024];
int g_len = 0;
void append(const char* format, ...)
{
    if (g_len >= int(sizeof g_text) - 1)
        return;
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(g_text + g_len, sizeof g_text - size_t(g_len), format, args);
    va_end(args);
    if (n > 0)
        g_len = std::min(int(sizeof g_text) - 1, g_len + n);
}

void appendLog()
{
    append("\nLast log lines:\n");
    const int next = g_next.load();
    for (int k = std::max(0, next - kLines); k < next; ++k)
        append("%s\n", g_log[k % kLines]);
}

#ifdef Q_OS_WIN
void writeReport(const char* what, EXCEPTION_POINTERS* ep, HANDLE thread, const CONTEXT* context);
#endif

void logHandler(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    const int i = g_next.fetch_add(1) % kLines;
    const QByteArray line = (QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz ")) + message).toUtf8().left(kLineLen - 1);
    std::memcpy(g_log[i], line.constData(), size_t(line.size()));
    g_log[i][line.size()] = 0;
    if (g_previous)
        g_previous(type, context, message);
#ifdef Q_OS_WIN
    // A fatal error (a failed Q_ASSERT, qFatal) ends the program with TerminateProcess on Windows, past the abort
    // handler: the report is written here.
    if (type == QtFatalMsg)
        writeReport("a fatal error", nullptr, nullptr, nullptr);
#endif
}

#ifdef Q_OS_WIN
const char* exceptionName(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "division by zero";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case 0xE06D7363: return "C++ exception";
    default: return "exception";
    }
}

// The thread's stack: module+offset of each frame (enough to look up later
// with the release's symbol files), and the function and line where the
// symbols (.pdb) are next to the program.
void appendStack(HANDLE thread, CONTEXT ctx)
{
#if defined(_M_X64)
    static const bool symbols = [] {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        return SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
    }();
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
    append("\nStack:\n");
    for (int i = 0; i < 40; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), thread, &frame, &ctx, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr)
            || !frame.AddrPC.Offset)
            break;
        const DWORD64 pc = frame.AddrPC.Offset;
        HMODULE module = nullptr;
        char path[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(pc), &module))
            GetModuleFileNameA(module, path, MAX_PATH);
        const char* name = std::strrchr(path, '\\');
        append("  %s+0x%llx", name ? name + 1 : path, static_cast<unsigned long long>(pc - reinterpret_cast<DWORD64>(module)));
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buffer);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 displacement = 0;
        if (symbols && SymFromAddr(GetCurrentProcess(), pc, &displacement, sym)) {
            append(" %s", sym->Name);
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof line;
            DWORD lineDisplacement = 0;
            if (SymGetLineFromAddr64(GetCurrentProcess(), pc, &lineDisplacement, &line)) {
                const char* file = std::strrchr(line.FileName, '\\');
                append(" (%s:%lu)", file ? file + 1 : line.FileName, line.LineNumber);
            }
        }
        append("\n");
    }
#else
    (void)thread;
    (void)ctx;
#endif
}

void writeReport(const char* what, EXCEPTION_POINTERS* ep, HANDLE thread = nullptr, const CONTEXT* context = nullptr)
{
    if (g_writing.exchange(true))
        return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t txt[MAX_PATH + 32], dmp[MAX_PATH + 32];
    swprintf(txt, MAX_PATH + 32, L"%ls%04d%02d%02d-%02d%02d%02d-%lu.txt", g_base, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId());
    swprintf(dmp, MAX_PATH + 32, L"%ls%04d%02d%02d-%02d%02d%02d-%lu.dmp", g_base, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId());
    append("%sCrashed: %s\nTime: %04d-%02d-%02d %02d:%02d:%02d\n", g_header, what, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    if (ep && ep->ExceptionRecord) {
        void* address = ep->ExceptionRecord->ExceptionAddress;
        HMODULE module = nullptr;
        char path[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(address), &module))
            GetModuleFileNameA(module, path, MAX_PATH);
        const char* name = std::strrchr(path, '\\');
        append("Exception: 0x%08lX (%s) at %s+0x%llx, thread %lu\n", ep->ExceptionRecord->ExceptionCode, exceptionName(ep->ExceptionRecord->ExceptionCode),
               name ? name + 1 : path, static_cast<unsigned long long>(static_cast<char*>(address) - reinterpret_cast<char*>(module)), GetCurrentThreadId());
    }
    if (ep && ep->ContextRecord)
        appendStack(GetCurrentThread(), *ep->ContextRecord);
    else if (context)
        appendStack(thread, *context);
    else { // (an abort, a fatal error, an unhandled C++ exception: where this thread is)
        CONTEXT here{};
        RtlCaptureContext(&here);
        appendStack(GetCurrentThread(), here);
    }
    appendLog();
    HANDLE f = CreateFileW(txt, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(f, g_text, DWORD(g_len), &written, nullptr);
        CloseHandle(f);
    }
    HANDLE d = CreateFileW(dmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (d != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), ep, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), d, MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
                          ep ? &info : nullptr, nullptr, nullptr);
        CloseHandle(d);
    }
}

LONG WINAPI onException(EXCEPTION_POINTERS* ep)
{
    writeReport("an exception", ep);
    return EXCEPTION_CONTINUE_SEARCH;
}
#else
void writeReport(const char* what, int signal)
{
    if (g_writing.exchange(true))
        return;
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char path[1100];
    std::snprintf(path, sizeof path, "%s%04d%02d%02d-%02d%02d%02d-%d.txt", g_base, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, int(getpid()));
    append("%sCrashed: %s (signal %d)\nTime: %04d-%02d-%02d %02d:%02d:%02d\n", g_header, what, signal, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
           tm.tm_hour, tm.tm_min, tm.tm_sec);
    appendLog();
    append("\nStack:\n");
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    [[maybe_unused]] const ssize_t written = ::write(fd, g_text, size_t(g_len));
    void* frames[64];
    backtrace_symbols_fd(frames, backtrace(frames, 64), fd);
    ::close(fd);
}

void onSignal(int signal)
{
    const char* what = signal == SIGSEGV ? "segmentation fault" : signal == SIGBUS ? "bus error" : signal == SIGFPE ? "arithmetic error"
                     : signal == SIGILL ? "illegal instruction" : "abort (a fatal error)";
    writeReport(what, signal);
    std::signal(signal, SIG_DFL);
    std::raise(signal);
}
#endif

} // namespace

QString folder()
{
    return QDir(brand::storageLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("crashes"));
}

void install()
{
    QDir().mkpath(folder());
    std::snprintf(g_header, sizeof g_header, "%s %s on %s (%s), Qt %s\n", qPrintable(QCoreApplication::applicationName()),
                  qPrintable(QCoreApplication::applicationVersion()), qPrintable(QSysInfo::prettyProductName()),
                  qPrintable(QSysInfo::currentCpuArchitecture()), qVersion());
    g_previous = qInstallMessageHandler(logHandler);
#ifdef Q_OS_WIN
    const std::wstring base = QDir::toNativeSeparators(QDir(folder()).filePath(QStringLiteral("crash-"))).toStdWString();
    wcsncpy_s(g_base, base.c_str(), _TRUNCATE);
    SetUnhandledExceptionFilter(onException);
    // A library loaded later (a GPU driver, a runtime) can put its own filter in ours' place, and a crash then left no
    // report: take it back every few seconds, naming the module that took it.
    auto* rearm = new QTimer(QCoreApplication::instance());
    QObject::connect(rearm, &QTimer::timeout, [] {
        const LPTOP_LEVEL_EXCEPTION_FILTER other = SetUnhandledExceptionFilter(onException);
        if (other == onException)
            return;
        HMODULE module = nullptr;
        char path[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(other), &module))
            GetModuleFileNameA(module, path, MAX_PATH);
        qWarning("crash handler: %s had replaced it; put back", path);
    });
    rearm->start(5000);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT); // no "abort() was called" box: the report says it
#ifdef _DEBUG
    // A failed assert (Qt's too) in a debug build: said on stderr and aborted (the report), not a box nobody answers in
    // a test, which then sat until its time ran out.
    for (int type : {_CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(type, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
        _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
    }
#endif
    std::signal(SIGABRT, [](int) {
        writeReport("abort (a fatal error)", nullptr);
        std::signal(SIGABRT, SIG_DFL);
        std::raise(SIGABRT);
    });
#else
    std::snprintf(g_base, sizeof g_base, "%s", QDir(folder()).filePath(QStringLiteral("crash-")).toUtf8().constData());
    for (int s : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT})
        std::signal(s, onSignal);
#endif
    std::set_terminate([] {
#ifdef Q_OS_WIN
        writeReport("an unhandled C++ exception", nullptr);
#else
        writeReport("an unhandled C++ exception", SIGABRT);
#endif
        std::abort();
    });
}

QString reportHang(unsigned long threadId)
{
#ifdef Q_OS_WIN
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, threadId);
    if (!thread)
        return {};
    SuspendThread(thread);
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(thread, &ctx))
        writeReport("a hang (the thread stopped answering)", nullptr, thread, &ctx);
    ResumeThread(thread);
    CloseHandle(thread);
    return QString::fromUtf8(g_text, g_len);
#else
    (void)threadId;
    return {};
#endif
}

QStringList newReports()
{
    const QDateTime seen = QSettings().value(QStringLiteral("crash/seen")).toDateTime();
    QStringList out;
    for (const QFileInfo& f : QDir(folder()).entryInfoList({QStringLiteral("crash-*.txt")}, QDir::Files, QDir::Time))
        if (!seen.isValid() || f.lastModified() > seen)
            out << f.filePath();
    return out;
}

void markSeen()
{
    QSettings().setValue(QStringLiteral("crash/seen"), QDateTime::currentDateTime());
}

QString summary(const QString& report)
{
    QFile f(report);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QStringList lines;
    for (const QString& line : QString::fromUtf8(f.readAll()).split(QLatin1Char('\n')))
        if (line.startsWith(QLatin1String("Crashed:")) || line.startsWith(QLatin1String("Exception:")))
            lines << line.trimmed();
    return lines.join(QLatin1Char(' '));
}

} // namespace crash
