#include "util/crash_handler.h"

#include <cstdio>
#include <mutex>
#include <optional>
#include <utility>
#include <format>
#include <string>

#include "midi/export/export_file.h"
#include "util/debugger.h"

#ifdef _WIN32
#include <windows.h>
// dbghelp must be included after windows.h
#include <dbghelp.h>
#endif

namespace andromeda::util {

namespace {

#ifdef _WIN32

const char* exception_name(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:      return "access violation";
    case EXCEPTION_STACK_OVERFLOW:        return "stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION:   return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "integer divide by zero";
    case EXCEPTION_PRIV_INSTRUCTION:      return "privileged instruction";
    case EXCEPTION_IN_PAGE_ERROR:         return "in-page error";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "misaligned access";
    default:                              return "exception";
    }
}

void report_line(const std::string& text) {
    Debugger::log_error(text);
    std::fputs(text.c_str(), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

LONG WINAPI on_unhandled_exception(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* record = info->ExceptionRecord;

    report_line("==================== CRASH ====================");
    report_line(std::format("{} (0x{:08X}) at 0x{:016X}", exception_name(record->ExceptionCode),
                            static_cast<std::uint32_t>(record->ExceptionCode),
                            reinterpret_cast<std::uintptr_t>(record->ExceptionAddress)));

    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        record->NumberParameters >= 2) {
        const char* what = record->ExceptionInformation[0] == 0   ? "reading"
                           : record->ExceptionInformation[0] == 1 ? "writing"
                                                                  : "executing";
        report_line(std::format("  while {} 0x{:016X}", what,
                                static_cast<std::uintptr_t>(record->ExceptionInformation[1])));
    }

    const HANDLE process = GetCurrentProcess();
    const HANDLE thread = GetCurrentThread();

    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(process, nullptr, TRUE);

    CONTEXT context = *info->ContextRecord;

    STACKFRAME64 frame{};
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;
#if defined(_M_X64)
    frame.AddrPC.Offset = context.Rip;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrStack.Offset = context.Rsp;
    constexpr DWORD machine = IMAGE_FILE_MACHINE_AMD64;
#elif defined(_M_ARM64)
    frame.AddrPC.Offset = context.Pc;
    frame.AddrFrame.Offset = context.Fp;
    frame.AddrStack.Offset = context.Sp;
    constexpr DWORD machine = IMAGE_FILE_MACHINE_ARM64;
#else
#error "unsupported architecture"
#endif

    alignas(SYMBOL_INFO) char symbol_storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbol_storage);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;

    for (int depth = 0; depth < 64; ++depth) {
        if (StackWalk64(machine, process, thread, &frame, &context, nullptr,
                        SymFunctionTableAccess64, SymGetModuleBase64, nullptr) == FALSE) {
            break;
        }
        if (frame.AddrPC.Offset == 0) {
            break;
        }

        const auto address = static_cast<DWORD64>(frame.AddrPC.Offset);

        std::string name = "??";
        DWORD64 displacement = 0;
        if (SymFromAddr(process, address, &displacement, symbol) != FALSE) {
            name = symbol->Name;
        }

        std::string where;
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
        DWORD line_displacement = 0;
        if (SymGetLineFromAddr64(process, address, &line_displacement, &line) != FALSE) {
            where = std::format("  {}:{}", line.FileName, line.LineNumber);
        }

        report_line(std::format("  #{:02} 0x{:016X} {}{}", depth,
                                static_cast<std::uintptr_t>(address), name, where));
    }

    SymCleanup(process);
    report_line("===============================================");

    return EXCEPTION_EXECUTE_HANDLER;
}

#endif

}

#ifdef _WIN32
LONG CALLBACK on_vectored_exception(EXCEPTION_POINTERS* info) {
    // the exporter writes through mapped views and handles a failing disk itself
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_IN_PAGE_ERROR &&
        andromeda::midi::exporter::writing_to_view()) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    switch (info->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_IN_PAGE_ERROR:
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }

    static bool reported = false;
    if (!reported) {
        reported = true;
        on_unhandled_exception(info);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

void install_crash_handler() {
#ifdef _WIN32
    // vectored handler needed: driver and crt replace the unhandled exception filter
    AddVectoredExceptionHandler(1, on_vectored_exception);
    SetUnhandledExceptionFilter(on_unhandled_exception);
#endif
}

}

namespace andromeda::util {

namespace {
std::mutex g_last_panic_mutex;
std::optional<std::string> g_last_panic;
}

void set_last_panic(std::string message) {
    std::lock_guard lock(g_last_panic_mutex);
    g_last_panic = std::move(message);
}

std::optional<std::string> take_last_panic() {
    std::lock_guard lock(g_last_panic_mutex);
    std::optional<std::string> out = std::move(g_last_panic);
    g_last_panic.reset();
    return out;
}

}
