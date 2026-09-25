// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "internal/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>

namespace site_fabric::internal {
namespace {

std::atomic<int> g_level{static_cast<int>(LogLevel::WARN)};

/// Guards the sink path and every write to it. The level is read without the
/// lock because it is an atomic; the sink is not, because two threads writing
/// one FILE would interleave.
std::mutex& sink_mutex() {
  static std::mutex mutex;
  return mutex;
}

std::string& sink_path() {
  static std::string path;
  return path;
}

const char* level_token(LogLevel level) {
  switch (level) {
    case LogLevel::ERROR:
      return "ERROR";
    case LogLevel::WARN:
      return "WARN";
    case LogLevel::INFO:
      return "INFO";
    case LogLevel::DEBUG:
      return "DEBUG";
  }
  return "UNKNOWN";
}

}  // namespace

void log_set_level(LogLevel level) {
  g_level.store(static_cast<int>(level), std::memory_order_relaxed);
}

LogLevel log_level() noexcept {
  return static_cast<LogLevel>(g_level.load(std::memory_order_relaxed));
}

void log_set_file(std::string path) {
  const std::lock_guard<std::mutex> guard(sink_mutex());
  sink_path() = std::move(path);
}

void log_message(LogLevel level, std::string_view component, std::string_view message) {
  if (static_cast<int>(level) > g_level.load(std::memory_order_relaxed)) {
    return;
  }
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  std::string line;
  line.reserve(component.size() + message.size() + 64);
  line += std::to_string(now);
  line += " [";
  line += level_token(level);
  line += "] ";
  line += component;
  line += ": ";
  line += message;
  line += '\n';

  const std::lock_guard<std::mutex> guard(sink_mutex());
  if (sink_path().empty()) {
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
    return;
  }
  std::FILE* stream = std::fopen(sink_path().c_str(), "ab");
  if (stream == nullptr) {
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
    return;
  }
  std::fwrite(line.data(), 1, line.size(), stream);
  std::fclose(stream);
}

}  // namespace site_fabric::internal
