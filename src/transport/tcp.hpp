// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Loopback TCP transport.
//
// Blocking sockets with an explicit receive timeout, so a peer that stops
// talking cannot hang a worker forever. Closing a listener from another thread
// interrupts an in-flight accept, which is what lets stop() join its workers
// without waiting for a connection that will never arrive.
//
// This is a real network transport: the multiprocess tests run separate
// operating-system processes against it over 127.0.0.1. It is not a
// same-process pipe dressed up as one.

#ifndef SITE_FABRIC_TRANSPORT_TCP_HPP
#define SITE_FABRIC_TRANSPORT_TCP_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "site_fabric/status.hpp"

namespace site_fabric::internal {

/// Reference-counted platform network start-up. Safe to call from any thread.
Status network_startup();
void network_shutdown();

class TcpConnection {
 public:
  TcpConnection() = default;
  ~TcpConnection();

  TcpConnection(const TcpConnection&) = delete;
  TcpConnection& operator=(const TcpConnection&) = delete;
  TcpConnection(TcpConnection&& other) noexcept;
  TcpConnection& operator=(TcpConnection&& other) noexcept;

  /// Reads exactly the requested bytes. Returns STOPPED on an orderly close
  /// before any byte arrived, TRUNCATED on a close mid-message, and BUSY on a
  /// receive timeout.
  Status read_exact(void* buffer, std::size_t bytes, std::int64_t timeout_ms);

  /// Writes every byte or fails. A short write is TRUNCATED.
  Status write_all(const void* buffer, std::size_t bytes, std::int64_t timeout_ms);

  /// Discards up to the bound without blocking, used before an error reply so
  /// the peer is not left mid-frame.
  Status drain(std::size_t max_bytes);

  void close();
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::string peer() const;
  [[nodiscard]] std::int64_t last_error() const noexcept { return last_error_; }

  [[nodiscard]] std::uintptr_t native_handle() const noexcept { return handle_; }
  void adopt(std::uintptr_t handle);

 private:
  std::uintptr_t handle_ = static_cast<std::uintptr_t>(-1);
  std::int64_t last_error_ = 0;
};

class TcpListener {
 public:
  TcpListener() = default;
  ~TcpListener();

  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;

  static Status listen_on(const std::string& host, std::uint16_t port, int backlog,
                          TcpListener& out);

  Status accept(TcpConnection& out);

  void close();
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] std::string host() const { return host_; }
  [[nodiscard]] std::string endpoint() const;

 private:
  std::uintptr_t handle_ = static_cast<std::uintptr_t>(-1);
  std::string host_;
  std::uint16_t port_ = 0;
};

/// Connects to a loopback endpoint with a bounded retry budget, so a test that
/// races a controller start does not fail spuriously and a genuinely dead
/// endpoint is reported rather than retried forever.
Status connect_with_retry(const std::string& host, std::uint16_t port,
                          std::int64_t connect_timeout_ms, int attempts, TcpConnection& out);

/// Writes one frame. The frame header is validated by the codec before a byte
/// is sent, so a payload above the bound never reaches the wire.
Status send_frame(TcpConnection& connection, std::uint16_t message_type,
                  std::span<const std::uint8_t> payload, std::int64_t timeout_ms);

/// Reads one frame. The header is read with header_timeout_ms so a caller can
/// poll its stop flag between frames, then the payload is read with
/// body_timeout_ms. A frame whose declared length exceeds the maximum is
/// refused before the buffer is allocated.
Status receive_frame(TcpConnection& connection, std::uint16_t& message_type,
                     std::vector<std::uint8_t>& payload, std::int64_t header_timeout_ms,
                     std::int64_t body_timeout_ms);

}  // namespace site_fabric::internal

#endif  // SITE_FABRIC_TRANSPORT_TCP_HPP
