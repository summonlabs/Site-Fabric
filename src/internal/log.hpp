// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Local diagnostics.
//
// Logging goes to stderr, or to a file the operator named explicitly. Nothing
// is transmitted anywhere. The level is process-global and is read without a
// lock, because the only writer is the operator's configuration call before
// any thread starts.

#ifndef SITE_FABRIC_INTERNAL_LOG_HPP
#define SITE_FABRIC_INTERNAL_LOG_HPP

#include <string>
#include <string_view>

namespace site_fabric::internal {

enum class LogLevel {
  ERROR = 0,
  WARN = 1,
  INFO = 2,
  DEBUG = 3,
};

void log_set_level(LogLevel level);
[[nodiscard]] LogLevel log_level() noexcept;
void log_set_file(std::string path);

void log_message(LogLevel level, std::string_view component, std::string_view message);

inline void log_error(std::string_view component, std::string_view message) {
  log_message(LogLevel::ERROR, component, message);
}
inline void log_warn(std::string_view component, std::string_view message) {
  log_message(LogLevel::WARN, component, message);
}
inline void log_info(std::string_view component, std::string_view message) {
  log_message(LogLevel::INFO, component, message);
}
inline void log_debug(std::string_view component, std::string_view message) {
  log_message(LogLevel::DEBUG, component, message);
}

}  // namespace site_fabric::internal

#endif  // SITE_FABRIC_INTERNAL_LOG_HPP
