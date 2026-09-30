#pragma once

#include <optional>
#include <string>

namespace andromeda::util {

void install_crash_handler();

void set_last_panic(std::string message);
std::optional<std::string> take_last_panic();

}
