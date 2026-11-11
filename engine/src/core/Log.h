#pragma once

#include <sstream>
#include <string>
#include <string_view>

namespace ke {

enum class LogLevel { Debug, Info, Warn, Error };

// Minimal deterministic logger: stderr for warnings/errors, stdout otherwise.
void logMessage(LogLevel level, std::string_view message);

// Debug lines are printed only when the environment variable KOTOR_LOG_DEBUG is set to a non-empty, non-0 value.
inline void logDebug(std::string_view message) { logMessage(LogLevel::Debug, message); }
inline void logInfo(std::string_view message) { logMessage(LogLevel::Info, message); }
inline void logWarn(std::string_view message) { logMessage(LogLevel::Warn, message); }
inline void logError(std::string_view message) { logMessage(LogLevel::Error, message); }

// Small helper to keep call sites readable without pulling in <format>.
template <typename... Parts>
std::string concat(Parts&&... parts)
{
    std::ostringstream stream;
    (stream << ... << parts);
    return stream.str();
}

} // namespace ke
