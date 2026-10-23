#pragma once

#include <sstream>
#include <string>
#include <string_view>

namespace ke {

enum class LogLevel { Info, Warn, Error };

// Minimal deterministic logger: stderr for warnings/errors, stdout otherwise.
void logMessage(LogLevel level, std::string_view message);

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
