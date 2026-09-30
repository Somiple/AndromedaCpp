#pragma once

#include <string>
#include <string_view>

namespace andromeda::util {

class Debugger {
public:
    static constexpr bool DEBUGGING_ENABLED = true;

    static void init_log_file(const std::string& path);

    static void log(std::string_view message);

    static void log_notime(std::string_view message);

    static void log_warning(std::string_view message);

    static void log_error(std::string_view message);

private:
    static void write_file(std::string_view message);
    static std::string timestamp();
};

}
