#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <string>

#include "app/main_window.h"
#include "util/crash_handler.h"
#include "util/debugger.h"
#include "version.h"

using andromeda::util::Debugger;

namespace {

std::mutex g_last_panic_mutex;
std::optional<std::string> g_last_panic;

void make_panic_hook() {
    std::set_terminate([]() {
        std::string msg = "Unknown panic";

        if (auto eptr = std::current_exception()) {
            try {
                std::rethrow_exception(eptr);
            } catch (const std::exception& e) {
                msg = e.what();
            } catch (...) {
            }
        }

        {
            std::lock_guard lock(g_last_panic_mutex);
            g_last_panic = "Panic: " + msg;
        }

        Debugger::log_error(*g_last_panic);
        std::abort();
    });
}

std::filesystem::path path_rel_to_abs(const std::string& path) {
    return std::filesystem::absolute(path);
}

}

int main(int argc, char** argv) {
    Debugger::init_log_file(path_rel_to_abs("./logs/debug.log").string());
    Debugger::log_notime("***** APPLICATION STARTED *****");

    (void)&make_panic_hook;

    andromeda::util::install_crash_handler();

    Debugger::log(std::format("Andromeda {}-{}", andromeda::EDITOR_VERSION,
                              andromeda::EDITOR_STAGE));

    andromeda::app::MainWindow::StartupOptions startup;

    try {
        andromeda::app::MainWindow main_window;
        main_window.set_startup_options(std::move(startup));
        return main_window.run();
    } catch (const std::exception& e) {
        const std::string msg = std::format("Fatal: {}", e.what());
        Debugger::log_error(msg);
        std::fputs(msg.c_str(), stderr);
        std::fputc('\n', stderr);
        std::fflush(stderr);
        return 1;
    } catch (...) {
        Debugger::log_error("Fatal: unknown exception");
        std::fputs("Fatal: unknown exception\n", stderr);
        std::fflush(stderr);
        return 1;
    }
}
