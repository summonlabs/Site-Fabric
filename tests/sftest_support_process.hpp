// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Process plumbing for the multiprocess tests.
//
// This is test scaffolding, not part of the runtime: it starts separate
// operating-system processes and, where the test calls for it, kills one. The
// kill is an input to the test (a crash is what is being modelled), never a way
// of turning a hang into a pass.

#ifndef SITE_FABRIC_TESTS_SFTEST_SUPPORT_PROCESS_HPP
#define SITE_FABRIC_TESTS_SFTEST_SUPPORT_PROCESS_HPP

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "site_fabric/site_fabric.hpp"
#include "transport/tcp.hpp"

namespace sftest {

[[nodiscard]] inline std::string quote_argument(const std::string& value) {
  std::string out = "\"";
  for (const char character : value) {
    if (character == '"') {
      out += "\\\"";
    } else {
      out += character;
    }
  }
  out += "\"";
  return out;
}

/// One child process, with its output drained by a reader thread so the child
/// can never block on a full pipe.
class ProcessRunner {
 public:
  ProcessRunner() = default;

  ~ProcessRunner() {
    if (running()) {
      kill_now();
    }
    wait_for_exit();
    close_handles();
  }

  ProcessRunner(const ProcessRunner&) = delete;
  ProcessRunner& operator=(const ProcessRunner&) = delete;

  bool spawn(const std::string& executable, const std::vector<std::string>& arguments) {
    close_handles();
    exit_code_ = 0;
    exited_.store(false);
    output_.clear();

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (::CreatePipe(&read_end, &write_end, &attributes, 0) == FALSE) {
      return false;
    }
    ::SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    std::string command_line = quote_argument(executable);
    for (const auto& argument : arguments) {
      command_line.push_back(' ');
      command_line += quote_argument(argument);
    }

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_end;
    startup.hStdError = write_end;
    startup.hStdInput = nullptr;

    PROCESS_INFORMATION info{};
    std::vector<char> mutable_line(command_line.begin(), command_line.end());
    mutable_line.push_back('\0');

    const BOOL created =
        ::CreateProcessA(nullptr, mutable_line.data(), nullptr, nullptr, TRUE,
                         CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
    ::CloseHandle(write_end);
    if (created == FALSE) {
      ::CloseHandle(read_end);
      return false;
    }

    process_ = info.hProcess;
    thread_ = info.hThread;
    pipe_ = read_end;

    reader_ = std::thread([this]() {
      char buffer[1024];
      DWORD got = 0;
      while (::ReadFile(pipe_, buffer, sizeof(buffer), &got, nullptr) != FALSE && got > 0) {
        const std::lock_guard<std::mutex> guard(output_mutex_);
        output_.append(buffer, got);
      }
    });
    return true;
  }

  [[nodiscard]] bool running() const {
    if (process_ == nullptr) {
      return false;
    }
    if (exited_.load()) {
      return false;
    }
    const DWORD wait = ::WaitForSingleObject(process_, 0);
    return wait == WAIT_TIMEOUT;
  }

  /// Terminates the process immediately. Used to model a crash.
  bool kill_now() {
    if (process_ == nullptr) {
      return false;
    }
    return ::TerminateProcess(process_, 137) != FALSE;
  }

  /// Waits for the process to exit, however long that takes. There is no
  /// timeout: a child that never exits is a defect to diagnose.
  void wait_for_exit() {
    if (process_ == nullptr) {
      return;
    }
    ::WaitForSingleObject(process_, INFINITE);
    DWORD code = 0;
    ::GetExitCodeProcess(process_, &code);
    exit_code_ = static_cast<int>(code);
    exited_.store(true);
    if (reader_.joinable()) {
      reader_.join();
    }
  }

  /// Waits up to the budget and reports whether the process exited.
  bool wait_with_budget(std::chrono::milliseconds budget) {
    if (process_ == nullptr) {
      return true;
    }
    const DWORD wait = ::WaitForSingleObject(process_, static_cast<DWORD>(budget.count()));
    if (wait != WAIT_OBJECT_0) {
      return false;
    }
    DWORD code = 0;
    ::GetExitCodeProcess(process_, &code);
    exit_code_ = static_cast<int>(code);
    exited_.store(true);
    if (reader_.joinable()) {
      reader_.join();
    }
    return true;
  }

  [[nodiscard]] int exit_code() const { return exit_code_; }

  /// Asks the child to stop by creating the file it watches, then waits for it.
  /// Nothing is killed: the child decides when to exit and reports its own
  /// exit code, so a clean shutdown is distinguishable from a crash.
  bool request_stop(const std::string& stop_file) {
    std::FILE* handle = std::fopen(stop_file.c_str(), "wb");
    if (handle == nullptr) {
      return false;
    }
    std::fputc('x', handle);
    std::fclose(handle);
    wait_for_exit();
    return true;
  }

  [[nodiscard]] std::string output() const {
    const std::lock_guard<std::mutex> guard(output_mutex_);
    return output_;
  }

 private:
  void close_handles() {
    if (thread_ != nullptr) {
      ::CloseHandle(thread_);
      thread_ = nullptr;
    }
    if (process_ != nullptr) {
      ::CloseHandle(process_);
      process_ = nullptr;
    }
    if (pipe_ != nullptr) {
      ::CloseHandle(pipe_);
      pipe_ = nullptr;
    }
  }

  HANDLE process_ = nullptr;
  HANDLE thread_ = nullptr;
  HANDLE pipe_ = nullptr;
  std::atomic<bool> exited_{false};
  int exit_code_ = 0;
  std::thread reader_;
  mutable std::mutex output_mutex_;
  std::string output_;
};

[[nodiscard]] inline std::uint16_t candidate_port(int attempt) {
  const DWORD pid = ::GetCurrentProcessId();
  const int base = 41000 + static_cast<int>(pid % 800) * 7;
  return static_cast<std::uint16_t>(base + attempt * 3);
}

/// Liveness probe: a raw TCP connect. The controller's own protocol would
/// answer a hello for the wrong site with SITE_MISMATCH, which is a successful
/// answer and not a reason to keep waiting.
[[nodiscard]] inline bool wait_for_controller_port(std::uint16_t port, int attempts) {
  for (int attempt = 0; attempt < attempts; ++attempt) {
    site_fabric::internal::TcpConnection probe;
    if (site_fabric::is_ok(
            site_fabric::internal::connect_with_retry("127.0.0.1", port, 500, 1, probe))) {
      probe.close();
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

/// Starts a controller process on a free-ish port, with a stop file that is
/// unique to this start.
///
/// The uniqueness matters: a stop file that outlives the controller it stopped
/// makes the next controller exit the moment it starts, which looks exactly
/// like a bind failure and wastes a long time looking like one.
[[nodiscard]] inline bool start_controller(const std::string& executable, const std::string& site,
                                           const std::string& stop_directory,
                                           const std::vector<std::string>& extra,
                                           ProcessRunner* runner, std::uint16_t& port,
                                           std::string& stop_file) {
  static std::atomic<unsigned long long> counter{0};
  stop_file = (std::filesystem::path(stop_directory) /
               ("stop-" + std::to_string(counter.fetch_add(1))))
                  .string();
  for (int attempt = 0; attempt < 24; ++attempt) {
    const std::uint16_t candidate = candidate_port(attempt);
    std::vector<std::string> arguments = {"--site",       site,     "--host", "127.0.0.1",
                                          "--port",       std::to_string(candidate),
                                          "--stop-file",  stop_file};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    if (!runner->spawn(executable, arguments)) {
      return false;
    }
    if (wait_for_controller_port(candidate, 40)) {
      port = candidate;
      return true;
    }
    (void)runner->kill_now();
    runner->wait_for_exit();
    std::fprintf(stderr, "start_controller: port %u never answered; exit=%d output=%s\n",
                 static_cast<unsigned>(candidate), runner->exit_code(),
                 runner->output().c_str());
  }
  return false;
}

[[nodiscard]] inline std::vector<std::string> member_arguments(
    std::uint16_t port, const std::string& site, const std::string& domain,
    const std::string& boot_nonce, std::uint64_t sequence, std::uint64_t capacity_bps,
    std::int64_t hold_ms, const std::string& scope = "ingress") {
  std::vector<std::string> arguments = {
      "--port",
      std::to_string(port),
      "--host",
      "127.0.0.1",
      "--site",
      site,
      "--domain",
      domain,
      "--publisher",
      "member-" + domain,
      "--boot-nonce",
      boot_nonce,
      "--capacity-bps",
      std::to_string(capacity_bps),
      "--hold-ms",
      std::to_string(hold_ms),
      "--scope",
      scope,
      "--quiet"};
  if (sequence != 0) {
    arguments.push_back("--sequence");
    arguments.push_back(std::to_string(sequence));
  }
  return arguments;
}

/// Starts a member process and leaves it running.
[[nodiscard]] inline bool start_member(const std::string& executable, std::uint16_t port,
                                       const std::string& site, const std::string& domain,
                                       const std::string& boot_nonce, std::uint64_t sequence,
                                       std::uint64_t capacity_bps, std::int64_t hold_ms,
                                       const std::string& scope, ProcessRunner* runner) {
  return runner->spawn(executable, member_arguments(port, site, domain, boot_nonce, sequence,
                                                    capacity_bps, hold_ms, scope));
}

/// Runs a member process to completion and returns its exit code.
[[nodiscard]] inline int run_member(const std::string& executable, std::uint16_t port,
                                    const std::string& site, const std::string& domain,
                                    const std::string& boot_nonce, std::uint64_t sequence,
                                    std::uint64_t capacity_bps, std::int64_t hold_ms,
                                    ProcessRunner* runner, const std::string& scope = "ingress") {
  if (!start_member(executable, port, site, domain, boot_nonce, sequence, capacity_bps, hold_ms,
                    scope, runner)) {
    return -1;
  }
  runner->wait_for_exit();
  return runner->exit_code();
}

/// Sends deliberately malformed bytes on a fresh connection.
[[nodiscard]] inline bool send_hostile_bytes(const std::string& host, std::uint16_t port,
                                             int variant) {
  site_fabric::internal::TcpConnection connection;
  if (!site_fabric::is_ok(
          site_fabric::internal::connect_with_retry(host, port, 1000, 5, connection))) {
    return false;
  }
  std::vector<std::uint8_t> bytes;
  switch (variant) {
    case 0:
      bytes = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
      break;
    case 1:
      bytes.assign(64, 0x00);
      break;
    case 2: {
      // A valid magic with a version this build does not speak.
      std::vector<std::uint8_t> frame;
      (void)site_fabric::encode_frame(site_fabric::MessageType::PING, {}, frame);
      frame[4] = 0x7F;
      bytes = frame;
      break;
    }
    case 3: {
      // A valid header whose declared length is far beyond the bound.
      std::vector<std::uint8_t> frame;
      (void)site_fabric::encode_frame(site_fabric::MessageType::PING, {}, frame);
      frame[12] = 0xFF;
      frame[13] = 0xFF;
      frame[14] = 0xFF;
      frame[15] = 0x7F;
      bytes = frame;
      break;
    }
    case 4: {
      // A well-formed header with a payload that does not match its digest.
      std::vector<std::uint8_t> frame;
      (void)site_fabric::encode_frame(site_fabric::MessageType::PING,
                                      std::vector<std::uint8_t>{1, 2, 3, 4}, frame);
      frame.push_back(0xAA);
      bytes = frame;
      break;
    }
    case 5: {
      // A truncated frame: header only, then close.
      std::vector<std::uint8_t> frame;
      (void)site_fabric::encode_frame(site_fabric::MessageType::PUBLISH_REQUEST,
                                      std::vector<std::uint8_t>(256, 0x5A), frame);
      bytes.assign(frame.begin(), frame.begin() + 20);
      break;
    }
    case 6: {
      // A frame whose header digest is wrong.
      std::vector<std::uint8_t> frame;
      (void)site_fabric::encode_frame(site_fabric::MessageType::PING, {}, frame);
      frame[20] ^= 0xFF;
      bytes = frame;
      break;
    }
    default: {
      // A publish frame carrying a declaration body that is not decodable.
      std::vector<std::uint8_t> frame;
      (void)site_fabric::encode_frame(site_fabric::MessageType::PING, {}, frame);
      bytes = frame;
      break;
    }
  }
  const site_fabric::Status written =
      connection.write_all(bytes.data(), bytes.size(), 2000);
  (void)written;
  connection.close();
  return true;
}

}  // namespace sftest

#endif  // _WIN32

#endif  // SITE_FABRIC_TESTS_SFTEST_SUPPORT_PROCESS_HPP
