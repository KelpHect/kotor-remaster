#include "core/Log.h"

#include <cstdlib>
#include <iostream>

namespace ke {

namespace {

const char* levelTag(LogLevel level)
{
    switch (level) {
    case LogLevel::Debug:
        return "DEBUG";
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Warn:
        return "WARN";
    case LogLevel::Error:
        return "ERROR";
    }
    return "INFO";
}

bool debugEnabled()
{
    static const bool enabled = [] {
        std::string text;
#if defined(_WIN32)
        char* value = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&value, &length, "KOTOR_LOG_DEBUG") == 0 && value != nullptr) {
            text = value;
            std::free(value);
        }
#else
        if (const char* value = std::getenv("KOTOR_LOG_DEBUG"); value != nullptr) {
            text = value;
        }
#endif
        return !text.empty() && text != "0";
    }();
    return enabled;
}

} // namespace

void logMessage(LogLevel level, std::string_view message)
{
    if (level == LogLevel::Debug && !debugEnabled()) {
        return;
    }
    std::ostream& stream = (level == LogLevel::Info || level == LogLevel::Debug) ? std::cout : std::cerr;
    stream << "[" << levelTag(level) << "] " << message << '\n';
    stream.flush();
}

} // namespace ke
