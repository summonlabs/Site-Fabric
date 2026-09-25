// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "site_fabric/publisher.hpp"

#include <string>
#include <vector>

#include "core/digest.hpp"
#include "site_fabric/limits.hpp"
#include "transport/tcp.hpp"

namespace site_fabric {

std::string PublisherSession::to_string() const {
  std::string out = established ? "established" : "not-established";
  out += " ";
  out += site_fabric::to_string(status);
  out += " epoch=" + epoch.to_string();
  out += " assigned=" + std::to_string(assigned_sequence);
  return out;
}

struct MemberPublisher::Impl {
  PublisherConfig config;
  internal::TcpConnection connection;
  PublisherSession session;
  Incarnation incarnation;
  std::uint64_t next_attempt_sequence = 1;
  std::uint32_t attempt = 0;

  /// Sends one request and reads the matching reply, translating a peer error
  /// frame into the status the caller would have received locally.
  [[nodiscard]] Status exchange(std::uint16_t request_type,
                                const std::vector<std::uint8_t>& request_payload,
                                std::uint16_t expected_response,
                                std::vector<std::uint8_t>& response_payload) {
    const Status sent = internal::send_frame(connection, request_type, request_payload,
                                             config.io_timeout_ms);
    if (!is_ok(sent)) {
      return sent;
    }
    std::uint16_t response_type = 0;
    const Status received = internal::receive_frame(connection, response_type, response_payload,
                                                    config.io_timeout_ms, config.io_timeout_ms);
    if (!is_ok(received)) {
      return received;
    }
    if (response_type == static_cast<std::uint16_t>(MessageType::ERROR_RESPONSE)) {
      ErrorResponse error;
      const Status decoded = decode_message(response_payload, error);
      if (!is_ok(decoded)) {
        return decoded;
      }
      return is_ok(error.status) ? Status::REFUSED : error.status;
    }
    if (response_type != expected_response) {
      return Status::MALFORMED;
    }
    return Status::OK;
  }
};

MemberPublisher::MemberPublisher(const PublisherConfig& config)
    : impl_(std::make_unique<Impl>()) {
  impl_->config = config;
  impl_->incarnation = config.incarnation;
}

MemberPublisher::~MemberPublisher() { close(); }

Status MemberPublisher::create(const PublisherConfig& config,
                               std::unique_ptr<MemberPublisher>& out) {
  if (config.port == 0) {
    return Status::INVALID;
  }
  if (!config.site.valid() || !config.domain.valid() || config.publisher.empty()) {
    return Status::INVALID;
  }
  if (!config.incarnation.boot.valid()) {
    return Status::INVALID;
  }
  if (config.io_timeout_ms <= 0 || config.connect_timeout_ms <= 0) {
    return Status::INVALID;
  }
  out = std::unique_ptr<MemberPublisher>(new MemberPublisher(config));
  return Status::OK;
}

bool MemberPublisher::connected() const noexcept { return impl_->connection.valid(); }

const PublisherSession& MemberPublisher::session() const noexcept { return impl_->session; }

const PublisherConfig& MemberPublisher::config() const noexcept { return impl_->config; }

Status MemberPublisher::connect() {
  if (connected()) {
    return Status::ALREADY_EXISTS;
  }
  if (!impl_->session.established) {
    impl_->incarnation = impl_->config.incarnation;
  }
  const Status connected_status =
      internal::connect_with_retry(impl_->config.host, impl_->config.port,
                                   impl_->config.connect_timeout_ms, 20, impl_->connection);
  if (!is_ok(connected_status)) {
    return connected_status;
  }
  return handshake(impl_->session);
}

Status MemberPublisher::handshake(PublisherSession& out) {
  if (!connected()) {
    return Status::NOT_STARTED;
  }
  HelloRequest request;
  request.domain = impl_->config.domain;
  request.site = impl_->config.site;
  request.incarnation = impl_->incarnation;
  request.publisher = impl_->config.publisher;
  request.observed_epoch = impl_->session.epoch;
  request.schema = impl_->config.schema;
  request.resume_from = Generation::unset();

  std::vector<std::uint8_t> payload;
  Status status = encode_message(request, payload);
  if (!is_ok(status)) {
    return status;
  }
  std::vector<std::uint8_t> response;
  status = impl_->exchange(static_cast<std::uint16_t>(MessageType::HELLO_REQUEST), payload,
                           static_cast<std::uint16_t>(MessageType::HELLO_RESPONSE), response);
  if (!is_ok(status)) {
    out.status = status;
    return status;
  }
  HelloResponse hello;
  status = decode_message(response, hello);
  if (!is_ok(status)) {
    out.status = status;
    return status;
  }
  out.status = hello.status;
  out.epoch = hello.epoch;
  out.controller = hello.controller;
  out.assigned_sequence = hello.assigned_sequence;
  out.controller_generation = hello.current_generation;
  out.factors = hello.factors;
  if (!is_ok(hello.status)) {
    out.established = false;
    return hello.status;
  }
  if (hello.assigned_sequence != 0) {
    impl_->incarnation.sequence = hello.assigned_sequence;
  }
  out.established = impl_->incarnation.valid();
  return out.established ? Status::OK : Status::INVALID;
}

Status MemberPublisher::publish(const MemberDomainDeclaration& declaration,
                                const PublishAttempt& attempt, PublishResponse& out) {
  if (!impl_->session.established) {
    return Status::NOT_STARTED;
  }
  MemberDomainDeclaration sealed = declaration;
  const Status sealed_status = sealed.seal();
  if (!is_ok(sealed_status)) {
    return sealed_status;
  }

  PublishAttempt effective = attempt;
  if (effective.sequence == 0) {
    effective.sequence = impl_->next_attempt_sequence++;
  }
  if (effective.attempt == 0) {
    effective.attempt = ++impl_->attempt;
  }

  PublishRequest request;
  request.declaration = sealed;
  request.attempt = effective;

  std::vector<std::uint8_t> payload;
  Status status = encode_message(request, payload);
  if (!is_ok(status)) {
    return status;
  }
  std::vector<std::uint8_t> response;
  status = impl_->exchange(static_cast<std::uint16_t>(MessageType::PUBLISH_REQUEST), payload,
                           static_cast<std::uint16_t>(MessageType::PUBLISH_RESPONSE), response);
  if (!is_ok(status)) {
    out.status = status;
    return status;
  }
  status = decode_message(response, out);
  if (!is_ok(status)) {
    return status;
  }
  return out.status;
}

Status MemberPublisher::retire(const RetireRequest& request, RetireResponse& out) {
  if (!impl_->session.established) {
    return Status::NOT_STARTED;
  }
  RetireRequestMessage message;
  message.request = request;
  message.request.incarnation = impl_->incarnation;

  std::vector<std::uint8_t> payload;
  Status status = encode_message(message, payload);
  if (!is_ok(status)) {
    return status;
  }
  std::vector<std::uint8_t> response;
  status = impl_->exchange(static_cast<std::uint16_t>(MessageType::RETIRE_REQUEST), payload,
                           static_cast<std::uint16_t>(MessageType::RETIRE_RESPONSE), response);
  if (!is_ok(status)) {
    out.status = status;
    return status;
  }
  status = decode_message(response, out);
  if (!is_ok(status)) {
    return status;
  }
  return out.status;
}

Status MemberPublisher::heartbeat(HeartbeatResponse& out) {
  if (!impl_->session.established) {
    return Status::NOT_STARTED;
  }
  HeartbeatRequest request;
  request.domain = impl_->config.domain;
  request.site = impl_->config.site;
  request.incarnation = impl_->incarnation;
  request.observed_epoch = impl_->session.epoch;
  request.since_sequence = 0;

  std::vector<std::uint8_t> payload;
  Status status = encode_message(request, payload);
  if (!is_ok(status)) {
    return status;
  }
  std::vector<std::uint8_t> response;
  status = impl_->exchange(static_cast<std::uint16_t>(MessageType::HEARTBEAT_REQUEST), payload,
                           static_cast<std::uint16_t>(MessageType::HEARTBEAT_RESPONSE), response);
  if (!is_ok(status)) {
    out.status = status;
    return status;
  }
  status = decode_message(response, out);
  if (!is_ok(status)) {
    return status;
  }
  if (is_ok(out.status)) {
    impl_->session.epoch = out.epoch;
  }
  return out.status;
}

Status MemberPublisher::fetch_site(std::uint64_t sequence, FetchSiteResponse& out) {
  FetchSiteRequest request;
  request.site = impl_->config.site;
  request.sequence = sequence;
  request.include_state = true;

  std::vector<std::uint8_t> payload;
  Status status = encode_message(request, payload);
  if (!is_ok(status)) {
    return status;
  }
  std::vector<std::uint8_t> response;
  status = impl_->exchange(static_cast<std::uint16_t>(MessageType::FETCH_SITE_REQUEST), payload,
                           static_cast<std::uint16_t>(MessageType::FETCH_SITE_RESPONSE), response);
  if (!is_ok(status)) {
    out.status = status;
    return status;
  }
  status = decode_message(response, out);
  if (!is_ok(status)) {
    return status;
  }
  return out.status;
}

Status MemberPublisher::ping(std::uint64_t nonce, std::uint64_t& echoed_nonce) {
  PingMessage request;
  request.nonce = nonce;
  std::vector<std::uint8_t> payload;
  Status status = encode_message(request, payload);
  if (!is_ok(status)) {
    return status;
  }
  std::vector<std::uint8_t> response;
  status = impl_->exchange(static_cast<std::uint16_t>(MessageType::PING), payload,
                           static_cast<std::uint16_t>(MessageType::PONG), response);
  if (!is_ok(status)) {
    return status;
  }
  PingMessage pong;
  status = decode_message(response, pong);
  if (!is_ok(status)) {
    return status;
  }
  echoed_nonce = pong.nonce;
  return Status::OK;
}

Status MemberPublisher::close() {
  impl_->connection.close();
  impl_->session.established = false;
  return Status::OK;
}

}  // namespace site_fabric
