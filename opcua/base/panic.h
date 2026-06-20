#pragma once

#include <source_location>
#include <string_view>

namespace opcua::base {

[[noreturn]] void Panic(
    std::string_view message,
    const std::source_location& location = std::source_location::current());

}  // namespace opcua::base
