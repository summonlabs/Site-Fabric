// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "transport/tcp.hpp"

#include "site_fabric/limits.hpp"
#include "site_fabric/protocol.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace site_fabric::internal {
namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
#endif

std::mutex& startup_mutex() {
  static std::mutex mutex;
  return mutex;
}

int& startup_count() {
  static int count = 0;
  return count;
}

[[nodiscard]] NativeSocket to_native(std::uintptr_t handle) {
  return static_cast<NativeSocket>(handle);
}

[[nodiscard]] std::uintptr_t to_handle(NativeSocket socket) {
  return static_cast<std::uintptr_t>(socket);
}

[[nodiscard]] bool is_valid_socket(NativeSocket socket) { return socket != kInvalidSocket; }

void close_socket(NativeSocket socket) {
  if (!is_valid_socket(socket)) {
    return;
  }
#ifdef _WIN32
  ::shutdown(socket, SD_BOTH);
  ::closesocket(socket);
#else
  ::shutdown(socket, SHUT_RDWR);
  ::close(socket);
#endif
}

void set_timeout(NativeSocket socket, std::int64_t timeout_ms) {
#ifdef _WIN32
  const DWORD timeout = static_cast<DWORD>(timeout_ms < 1 ? 1 : timeout_ms);
  ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
               sizeof(timeout));
  ::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout),
               sizeof(timeout));
#else
  timeval timeout{};
  timeout.tv_sec = static_cast<long>(timeout_ms / 1000);
  timeout.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);
  if (timeout.tv_sec == 0 && timeout.tv_usec == 0) {
    timeout.tv_usec = 1000;
  }
  ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  ::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
}

/// True when the last error means "the peer timed out" rather than "the peer
/// is gone". The two produce different statuses.
[[nodiscard]] bool last_error_is_timeout() {
#ifdef _WIN32
  const int code = ::WSAGetLastError();
  return code == WSAETIMEDOUT || code == WSAEWOULDBLOCK;
#else
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

[[nodiscard]] bool last_error_is_interrupted() {
#ifdef _WIN32
  return ::WSAGetLastError() == WSAEINTR;
#else
  return errno == EINTR;
#endif
}

}  // namespace

Status network_startup() {
  const std::lock_guard<std::mutex> guard(startup_mutex());
  if (startup_count() == 0) {
#ifdef _WIN32
    WSADATA data{};
    if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      return Status::UNSUPPORTED;
    }
#endif
  }
  ++startup_count();
  return Status::OK;
}

void network_shutdown() {
  const std::lock_guard<std::mutex> guard(startup_mutex());
  if (startup_count() == 0) {
    return;
  }
  --startup_count();
  if (startup_count() == 0) {
#ifdef _WIN32
    ::WSACleanup();
#endif
  }
}

TcpConnection::~TcpConnection() { close(); }

TcpConnection::TcpConnection(TcpConnection&& other) noexcept
    : handle_(other.handle_), last_error_(other.last_error_) {
  other.handle_ = static_cast<std::uintptr_t>(-1);
  other.last_error_ = 0;
}

TcpConnection& TcpConnection::operator=(TcpConnection&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    last_error_ = other.last_error_;
    other.handle_ = static_cast<std::uintptr_t>(-1);
    other.last_error_ = 0;
  }
  return *this;
}

bool TcpConnection::valid() const noexcept {
  return is_valid_socket(to_native(handle_));
}

void TcpConnection::adopt(std::uintptr_t handle) {
  close();
  handle_ = handle;
}

void TcpConnection::close() {
  NativeSocket socket = to_native(handle_);
  if (is_valid_socket(socket)) {
    close_socket(socket);
  }
  handle_ = static_cast<std::uintptr_t>(-1);
}

Status TcpConnection::read_exact(void* buffer, std::size_t bytes, std::int64_t timeout_ms) {
  if (!valid()) {
    return Status::STOPPED;
  }
  NativeSocket socket = to_native(handle_);
  set_timeout(socket, timeout_ms);

  auto* cursor = static_cast<std::uint8_t*>(buffer);
  std::size_t received = 0;
  while (received < bytes) {
    const std::size_t want = bytes - received;
    const int chunk = static_cast<int>(want > 1U << 20U ? 1U << 20U : want);
    const int got = ::recv(socket, reinterpret_cast<char*>(cursor + received), chunk, 0);
    if (got > 0) {
      received += static_cast<std::size_t>(got);
      continue;
    }
    if (got == 0) {
      return received == 0 ? Status::STOPPED : Status::TRUNCATED;
    }
    if (last_error_is_interrupted()) {
      continue;
    }
    if (last_error_is_timeout()) {
      return Status::BUSY;
    }
    last_error_ = 1;
    return Status::STOPPED;
  }
  return Status::OK;
}

Status TcpConnection::write_all(const void* buffer, std::size_t bytes, std::int64_t timeout_ms) {
  if (!valid()) {
    return Status::STOPPED;
  }
  NativeSocket socket = to_native(handle_);
  set_timeout(socket, timeout_ms);

  const auto* cursor = static_cast<const std::uint8_t*>(buffer);
  std::size_t sent = 0;
  while (sent < bytes) {
    const std::size_t want = bytes - sent;
    const int chunk = static_cast<int>(want > 1U << 20U ? 1U << 20U : want);
    const int put = ::send(socket, reinterpret_cast<const char*>(cursor + sent), chunk, 0);
    if (put > 0) {
      sent += static_cast<std::size_t>(put);
      continue;
    }
    if (last_error_is_interrupted()) {
      continue;
    }
    last_error_ = 2;
    return Status::TRUNCATED;
  }
  return Status::OK;
}

Status TcpConnection::drain(std::size_t max_bytes) {
  if (!valid()) {
    return Status::STOPPED;
  }
  NativeSocket socket = to_native(handle_);
  set_timeout(socket, 1);
  std::vector<std::uint8_t> scratch(1024);
  std::size_t drained = 0;
  while (drained < max_bytes) {
    const int got = ::recv(socket, reinterpret_cast<char*>(scratch.data()),
                           static_cast<int>(scratch.size()), 0);
    if (got <= 0) {
      break;
    }
    drained += static_cast<std::size_t>(got);
  }
  return Status::OK;
}

std::string TcpConnection::peer() const {
  if (!valid()) {
    return {};
  }
  sockaddr_storage address{};
  int length = static_cast<int>(sizeof(address));
  if (::getpeername(to_native(handle_), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    return {};
  }
  char text[INET6_ADDRSTRLEN] = {};
  if (address.ss_family == AF_INET) {
    const auto* v4 = reinterpret_cast<const sockaddr_in*>(&address);
    ::inet_ntop(AF_INET, &v4->sin_addr, text, sizeof(text));
    return std::string(text) + ":" + std::to_string(ntohs(v4->sin_port));
  }
  if (address.ss_family == AF_INET6) {
    const auto* v6 = reinterpret_cast<const sockaddr_in6*>(&address);
    ::inet_ntop(AF_INET6, &v6->sin6_addr, text, sizeof(text));
    return std::string(text) + ":" + std::to_string(ntohs(v6->sin6_port));
  }
  return {};
}

TcpListener::~TcpListener() { close(); }

bool TcpListener::valid() const noexcept { return is_valid_socket(to_native(handle_)); }

std::string TcpListener::endpoint() const {
  return host_ + ":" + std::to_string(port_);
}

void TcpListener::close() {
  NativeSocket socket = to_native(handle_);
  if (is_valid_socket(socket)) {
    close_socket(socket);
  }
  handle_ = static_cast<std::uintptr_t>(-1);
}

Status TcpListener::listen_on(const std::string& host, std::uint16_t port, int backlog,
                              TcpListener& out) {
  const Status startup = network_startup();
  if (!is_ok(startup)) {
    return startup;
  }

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_PASSIVE;

  addrinfo* resolved = nullptr;
  const std::string service = std::to_string(port);
  const int resolved_status = ::getaddrinfo(host.empty() ? "127.0.0.1" : host.c_str(),
                                            service.c_str(), &hints, &resolved);
  if (resolved_status != 0 || resolved == nullptr) {
    return Status::INVALID;
  }

  NativeSocket socket = kInvalidSocket;
  for (addrinfo* candidate = resolved; candidate != nullptr; candidate = candidate->ai_next) {
    socket = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (!is_valid_socket(socket)) {
      continue;
    }
    const int reuse = 1;
    ::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                 sizeof(reuse));
    if (::bind(socket, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0) {
      break;
    }
    close_socket(socket);
    socket = kInvalidSocket;
  }
  ::freeaddrinfo(resolved);

  if (!is_valid_socket(socket)) {
    return Status::INVALID;
  }
  if (::listen(socket, backlog) != 0) {
    close_socket(socket);
    return Status::INVALID;
  }

  sockaddr_storage bound{};
  int bound_length = static_cast<int>(sizeof(bound));
  if (::getsockname(socket, reinterpret_cast<sockaddr*>(&bound), &bound_length) != 0) {
    close_socket(socket);
    return Status::INVALID;
  }

  out.close();
  out.handle_ = to_handle(socket);
  out.host_ = host.empty() ? "127.0.0.1" : host;
  if (bound.ss_family == AF_INET) {
    out.port_ = ntohs(reinterpret_cast<const sockaddr_in*>(&bound)->sin_port);
  } else {
    out.port_ = port;
  }
  return Status::OK;
}

Status TcpListener::accept(TcpConnection& out) {
  if (!valid()) {
    return Status::STOPPED;
  }
  sockaddr_storage address{};
  int length = static_cast<int>(sizeof(address));
  const NativeSocket accepted =
      ::accept(to_native(handle_), reinterpret_cast<sockaddr*>(&address), &length);
  if (!is_valid_socket(accepted)) {
    if (last_error_is_interrupted()) {
      return Status::BUSY;
    }
    return Status::STOPPED;
  }
  const int nodelay = 1;
  ::setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay),
               sizeof(nodelay));
  out.close();
  out.adopt(to_handle(accepted));
  return Status::OK;
}

Status connect_with_retry(const std::string& host, std::uint16_t port,
                          std::int64_t connect_timeout_ms, int attempts, TcpConnection& out) {
  const Status startup = network_startup();
  if (!is_ok(startup)) {
    return startup;
  }
  if (attempts < 1) {
    attempts = 1;
  }

  Status last = Status::STOPPED;
  for (int attempt = 0; attempt < attempts; ++attempt) {
    NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (!is_valid_socket(socket)) {
      last = Status::UNSUPPORTED;
      continue;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
      close_socket(socket);
      last = Status::INVALID;
      continue;
    }
    if (::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
      const int nodelay = 1;
      ::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay),
                   sizeof(nodelay));
      out.close();
      out.adopt(to_handle(socket));
      return Status::OK;
    }
    close_socket(socket);
    last = Status::NOT_FOUND;
    if (attempt + 1 < attempts) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  (void)connect_timeout_ms;
  return last;
}

Status send_frame(TcpConnection& connection, std::uint16_t message_type,
                  std::span<const std::uint8_t> payload, std::int64_t timeout_ms) {
  if (message_type == 0 ||
      message_type > static_cast<std::uint16_t>(MessageType::PONG)) {
    return Status::UNSUPPORTED;
  }
  std::vector<std::uint8_t> frame;
  const Status encoded = encode_frame(static_cast<MessageType>(message_type), payload, frame);
  if (!is_ok(encoded)) {
    return encoded;
  }
  return connection.write_all(frame.data(), frame.size(), timeout_ms);
}

Status receive_frame(TcpConnection& connection, std::uint16_t& message_type,
                     std::vector<std::uint8_t>& payload, std::int64_t header_timeout_ms,
                     std::int64_t body_timeout_ms) {
  payload.clear();
  std::uint8_t header_bytes[kFrameHeaderBytes] = {};
  const Status header_status =
      connection.read_exact(header_bytes, sizeof(header_bytes), header_timeout_ms);
  if (!is_ok(header_status)) {
    return header_status;
  }
  FrameHeader header;
  const Status decoded =
      decode_frame_header(std::span<const std::uint8_t>(header_bytes, sizeof(header_bytes)),
                          header);
  if (!is_ok(decoded)) {
    return decoded;
  }
  message_type = static_cast<std::uint16_t>(header.type);
  payload.resize(header.payload_bytes);
  if (header.payload_bytes != 0) {
    const Status body_status =
        connection.read_exact(payload.data(), payload.size(), body_timeout_ms);
    if (!is_ok(body_status)) {
      payload.clear();
      return body_status;
    }
  }
  const Status verified =
      verify_payload(header, std::span<const std::uint8_t>(payload.data(), payload.size()));
  if (!is_ok(verified)) {
    payload.clear();
    return verified;
  }
  return Status::OK;
}

}  // namespace site_fabric::internal
