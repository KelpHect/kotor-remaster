#include "core/Log.h"

#include <iostream>

namespace ke {

namespace {

const char* levelTag(LogLevel level)
{
    switch (level) {
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Warn:
        return "WARN";
    case LogLevel::Error:
        return "ERROR";
    }
    return "INFO";
}

} // namespace

void logMessage(LogLevel level, std::string_view message)
{
    std::ostream& stream = (level == LogLevel::Info) ? std::cout : std::cerr;
    stream << "[" << levelTag(level) << "] " << message << '\n';
    stream.flush();
}

} // namespace ke
