// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Framing and message codecs.
//
// A frame is validated in the order that costs least and reveals least: magic,
// then version, then type, then length, then digest, and only then is a payload
// decoded. A frame whose declared length exceeds the configured maximum is
// refused before any buffer of that size is created.
//
// The writers below are deliberately explicit rather than reflective. The wire
// layout is an interface: a reader that has to infer a field order is a reader
// that will one day infer it wrong.

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "core/digest.hpp"
#include "internal/crypto.hpp"
#include "site_fabric/limits.hpp"
#include "site_fabric/protocol.hpp"

namespace site_fabric {
namespace {

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

[[nodiscard]] std::uint16_t get_u16(const std::uint8_t* data) {
  return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(data[0]) |
      static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[1]) << 8U));
}

[[nodiscard]] std::uint32_t get_u32(const std::uint8_t* data) {
  return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8U) |
         (static_cast<std::uint32_t>(data[2]) << 16U) |
         (static_cast<std::uint32_t>(data[3]) << 24U);
}

// --- Field helpers shared by the writers -----------------------------------

void write_domain_list(internal::CanonicalWriter& writer,
                       const std::vector<MemberDomainKey>& domains) {
  writer.count(domains.size());
  for (const auto& domain : domains) {
    internal::write_member_domain_key(writer, domain);
  }
}

[[nodiscard]] Status read_domain_list(internal::CanonicalReader& reader,
                                      std::vector<MemberDomainKey>& out) {
  std::size_t count = 0;
  Status status = reader.count(count, limits::kMaxMemberDomains + 1);
  if (!is_ok(status)) {
    return status;
  }
  out.resize(count);
  for (auto& domain : out) {
    status = internal::read_member_domain_key(reader, domain);
    if (!is_ok(status)) {
      return status;
    }
  }
  return Status::OK;
}

}  // namespace

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

std::string FrameHeader::to_string() const {
  std::string out = site_fabric::to_string(type);
  out += " v" + std::to_string(protocol_version);
  out += " bytes=" + std::to_string(payload_bytes);
  out += " flags=" + std::to_string(flags);
  out += " digest=" + payload_digest.short_hex();
  return out;
}

Status encode_frame(MessageType type, std::span<const std::uint8_t> payload,
                    std::vector<std::uint8_t>& out, std::uint32_t flags) {
  if (!is_known(type)) {
    return Status::UNSUPPORTED;
  }
  if (payload.size() > limits::kMaxFrameBytes) {
    return Status::LIMIT_EXCEEDED;
  }

  out.clear();
  out.reserve(kFrameHeaderBytes + payload.size());
  put_u32(out, kFrameMagic);
  put_u16(out, kWireProtocolVersion);
  put_u16(out, static_cast<std::uint16_t>(type));
  put_u32(out, flags);
  put_u32(out, static_cast<std::uint32_t>(payload.size()));

  const Digest payload_digest = internal::digest_bytes(payload);
  out.insert(out.end(), payload_digest.bytes.begin(), payload_digest.bytes.end());
  out.insert(out.end(), payload.begin(), payload.end());
  return Status::OK;
}

Status decode_frame_header(std::span<const std::uint8_t> header_bytes, FrameHeader& out) {
  if (header_bytes.size() != kFrameHeaderBytes) {
    return Status::TRUNCATED;
  }
  const std::uint8_t* data = header_bytes.data();
  if (get_u32(data) != kFrameMagic) {
    return Status::MALFORMED;
  }
  const std::uint16_t version = get_u16(data + 4);
  if (version != kWireProtocolVersion) {
    return Status::UNSUPPORTED_FORMAT;
  }
  const std::uint16_t type = get_u16(data + 6);
  if (type == 0 || type > static_cast<std::uint16_t>(MessageType::PONG)) {
    return Status::UNSUPPORTED;
  }
  const std::uint32_t payload_bytes = get_u32(data + 12);
  if (payload_bytes > limits::kMaxFrameBytes) {
    return Status::LIMIT_EXCEEDED;
  }

  out.protocol_version = version;
  out.type = static_cast<MessageType>(type);
  out.flags = get_u32(data + 8);
  out.payload_bytes = payload_bytes;
  std::memcpy(out.payload_digest.bytes.data(), data + 16, out.payload_digest.bytes.size());
  return Status::OK;
}

Status verify_payload(const FrameHeader& header, std::span<const std::uint8_t> payload) {
  if (payload.size() != header.payload_bytes) {
    return Status::TRUNCATED;
  }
  const Digest computed = internal::digest_bytes(payload);
  if (!(computed == header.payload_digest)) {
    return Status::INTEGRITY_FAILURE;
  }
  return Status::OK;
}

// ---------------------------------------------------------------------------
// Message codecs
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] Status decode_factors(internal::CanonicalReader& reader, Factors& out) {
  return internal::read_factors(reader, out);
}

}  // namespace

std::string HelloRequest::to_string() const {
  return domain.to_string() + " site=" + site.value() + " incarnation=" +
         incarnation.to_string() + " publisher=" + publisher.value();
}

Status encode_message(const HelloRequest& message, std::vector<std::uint8_t>& out) {
  internal::CanonicalWriter writer;
  internal::write_member_domain_key(writer, message.domain);
  writer.text(message.site.value());
  writer.incarnation(message.incarnation);
  writer.text(message.publisher.value());
  writer.epoch(message.observed_epoch);
  writer.schema(message.schema);
  writer.generation(message.resume_from);
  out = writer.buffer();
  return Status::OK;
}

Status decode_message(std::span<const std::uint8_t> payload, HelloRequest& out) {
  internal::CanonicalReader reader(payload);
  Status status = internal::read_member_domain_key(reader, out.domain);
  if (!is_ok(status)) {
    return status;
  }
  status = internal::read_strong_id(reader, out.site);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.incarnation(out.incarnation);
  if (!is_ok(status)) {
    return status;
  }
  status = internal::read_strong_id(reader, out.publisher);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.epoch(out.observed_epoch);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.schema(out.schema);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.generation(out.resume_from);
  if (!is_ok(status)) {
    return status;
  }
  return reader.finish();
}

MessageType message_type_of(const HelloRequest&) { return MessageType::HELLO_REQUEST; }

std::string HelloResponse::to_string() const {
  return std::string(site_fabric::to_string(status)) + " epoch=" + epoch.to_string() +
         " assigned=" + std::to_string(assigned_sequence);
}

Status encode_message(const HelloResponse& message, std::vector<std::uint8_t>& out) {
  internal::CanonicalWriter writer;
  writer.u8(static_cast<std::uint8_t>(message.status));
  writer.text(message.site.value());
  writer.epoch(message.epoch);
  writer.incarnation(message.controller);
  writer.u64(message.assigned_sequence);
  writer.generation(message.current_generation);
  internal::write_factors(writer, message.factors);
  out = writer.buffer();
  return Status::OK;
}

Status decode_message(std::span<const std::uint8_t> payload, HelloResponse& out) {
  internal::CanonicalReader reader(payload);
  std::uint8_t status_code = 0;
  Status status = reader.u8(status_code);
  if (!is_ok(status)) {
    return status;
  }
  if (status_code > static_cast<std::uint8_t>(Status::NO_AUTHORITATIVE_SOURCE)) {
    return Status::MALFORMED;
  }
  out.status = static_cast<Status>(status_code);
  status = internal::read_strong_id(reader, out.site);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.epoch(out.epoch);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.incarnation(out.controller);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.u64(out.assigned_sequence);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.generation(out.current_generation);
  if (!is_ok(status)) {
    return status;
  }
  status = decode_factors(reader, out.factors);
  if (!is_ok(status)) {
    return status;
  }
  return reader.finish();
}

MessageType message_type_of(const HelloResponse&) { return MessageType::HELLO_RESPONSE; }

std::string PublishRequest::to_string() const {
  return declaration.to_string() + " attempt=" + attempt.to_string();
}

Status encode_message(const PublishRequest& message, std::vector<std::uint8_t>& out) {
  internal::CanonicalWriter writer;
  internal::write_declaration(writer, message.declaration);
  writer.u64(message.attempt.sequence);
  writer.u32(message.attempt.attempt);
  out = writer.buffer();
  return Status::OK;
}

Status decode_message(std::span<const std::uint8_t> payload, PublishRequest& out) {
  internal::CanonicalReader reader(payload);
  Status status = internal::read_declaration(reader, out.declaration);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.u64(out.attempt.sequence);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.u32(out.attempt.attempt);
  if (!is_ok(status)) {
    return status;
  }
  return reader.finish();
}

MessageType message_type_of(const PublishRequest&) { return MessageType::PUBLISH_REQUEST; }

std::string PublishResponse::to_string() const {
  return std::string(site_fabric::to_string(status)) + " epoch=" + epoch.to_string() +
         " site_generation=" + site_generation.to_string() +
         " accepted=" + std::to_string(acceptance_sequence);
}

Status encode_message(const PublishResponse& message, std::vector<std::uint8_t>& out) {
  internal::CanonicalWriter writer;
  writer.u8(static_cast<std::uint8_t>(message.status));
  writer.text(message.site.value());
  writer.epoch(message.epoch);
  writer.generation(message.site_generation);
  writer.u64(message.acceptance_sequence);
  writer.flag(message.recomposed);
  internal::write_factors(writer, message.factors);
  out = writer.buffer();
  return Status::OK;
}

Status decode_message(std::span<const std::uint8_t> payload, PublishResponse& out) {
  internal::CanonicalReader reader(payload);
  std::uint8_t status_code = 0;
  Status status = reader.u8(status_code);
  if (!is_ok(status)) {
    return status;
  }
  if (status_code > static_cast<std::uint8_t>(Status::NO_AUTHORITATIVE_SOURCE)) {
    return Status::MALFORMED;
  }
  out.status = static_cast<Status>(status_code);
  status = internal::read_strong_id(reader, out.site);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.epoch(out.epoch);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.generation(out.site_generation);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.u64(out.acceptance_sequence);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.flag(out.recomposed);
  if (!is_ok(status)) {
    return status;
  }
  status = decode_factors(reader, out.factors);
  if (!is_ok(status)) {
    return status;
  }
  return reader.finish();
}

MessageType message_type_of(const PublishResponse&) { return MessageType::PUBLISH_RESPONSE; }

std::string RetireRequestMessage::to_string() const { return request.domain.to_string(); }

Status encode_message(const RetireRequestMessage& message, std::vector<std::uint8_t>& out) {
  internal::CanonicalWriter writer;
  internal::write_member_domain_key(writer, message.request.domain);
  writer.generation(message.request.generation);
  writer.incarnation(message.request.incarnation);
  writer.text(message.request.reason);
  out = writer.buffer();
  return Status::OK;
}

Status decode_message(std::span<const std::uint8_t> payload, RetireRequestMessage& out) {
  internal::CanonicalReader reader(payload);
  Status status = internal::read_member_domain_key(reader, out.request.domain);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.generation(out.request.generation);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.incarnation(out.request.incarnation);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.text(out.request.reason, limits::kMaxTextBytes);
  if (!is_ok(status)) {
    return status;
  }
  return reader.finish();
}

MessageType message_type_of(const RetireRequestMessage&) { return MessageType::RETIRE_REQUEST; }

std::string RetireResponse::to_string() const {
  return std::string(site_fabric::to_string(status)) + " accepted=" +
         std::to_string(acceptance_sequence);
}

Status encode_message(const RetireResponse& message, std::vector<std::uint8_t>& out) {
  internal::CanonicalWriter writer;
  writer.u8(static_cast<std::uint8_t>(message.status));
  writer.text(message.site.value());
  writer.epoch(message.epoch);
  writer.u64(message.acceptance_sequence);
  internal::write_factors(writer, message.factors);
  out = writer.buffer();
  return Status::OK;
}

Status decode_message(std::span<const std::uint8_t> payload, RetireResponse& out) {
  internal::CanonicalReader reader(payload);
  std::uint8_t status_code = 0;
  Status status = reader.u8(status_code);
  if (!is_ok(status)) {
    return status;
  }
  if (status_code > static_cast<std::uint8_t>(Status::NO_AUTHORITATIVE_SOURCE)) {
    return Status::MALFORMED;
  }
  out.status = static_cast<Status>(status_code);
  status = internal::read_strong_id(reader, out.site);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.epoch(out.epoch);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.u64(out.acceptance_sequence);
  if (!is_ok(status)) {
    return status;
  }
  status = decode_factors(reader, out.factors);
  if (!is_ok(status)) {
    return status;
  }
  return reader.finish();
}

MessageType message_type_of(const RetireResponse&) { return MessageType::RETIRE_RESPONSE; }

std::string FetchSiteRequest::to_string() const {
  return "site=" + site.value() + " sequence=" + std::to_string(sequence);
}

Status encode_message(const FetchSiteRequest& message, std::vector<std::uint8_t>& out) {
  internal::CanonicalWriter writer;
  writer.text(message.site.value());
  writer.u64(message.sequence);
  writer.flag(message.include_state);
  out = writer.buffer();
  return Status::OK;
}

Status decode_message(std::span<const std::uint8_t> payload, FetchSiteRequest& out) {
  internal::CanonicalReader reader(payload);
  Status status = internal::read_strong_id(reader, out.site);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.u64(out.sequence);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.flag(out.include_state);
  if (!is_ok(status)) {
    return status;
  }
  return reader.finish();
}

MessageType message_type_of(const FetchSiteRequest&) { return MessageType::FETCH_SITE_REQUEST; }

std::string FetchSiteResponse::to_string() const {
  return std::string(site_fabric::to_string(status)) +
         " has_snapshot=" + (has_snapshot ? "true" : "false") +
         " sequence=" + std::to_string(snapshot.sequence);
}

Status encode_message(const FetchSiteResponse& message, std::vector<std::uint8_t>& out) {
  internal::CanonicalWriter writer;
  writer.u8(static_cast<std::uint8_t>(message.status));
  writer.flag(message.has_snapshot);
  if (message.has_snapshot) {
    internal::write_snapshot_body(writer, message.snapshot);
  }
  internal::write_factors(writer, message.factors);
  out = writer.buffer();
  return Status::OK;
}

Status decode_message(std::span<const std::uint8_t> payload, FetchSiteResponse& out) {
  internal::CanonicalReader reader(payload);
  std::uint8_t status_code = 0;
  Status status = reader.u8(status_code);
  if (!is_ok(status)) {
    return status;
  }
  if (status_code > static_cast<std::uint8_t>(Status::NO_AUTHORITATIVE_SOURCE)) {
    return Status::MALFORMED;
  }
  out.status = static_cast<Status>(status_code);
  status = reader.flag(out.has_snapshot);
  if (!is_ok(status)) {
    return status;
  }
  if (out.has_snapshot) {
    status = internal::read_snapshot_body(reader, out.snapshot);
    if (!is_ok(status)) {
      return status;
    }
  }
  status = decode_factors(reader, out.factors);
  if (!is_ok(status)) {
    return status;
  }
  return reader.finish();
}

MessageType message_type_of(const FetchSiteResponse&) { return MessageType::FETCH_SITE_RESPONSE; }

std::string HeartbeatRequest::to_string() const {
  return domain.to_string() + " incarnation=" + incarnation.to_string();
}

Status encode_message(const HeartbeatRequest& message, std::vector<std::uint8_t>& out) {
  internal::CanonicalWriter writer;
  internal::write_member_domain_key(writer, message.domain);
  writer.text(message.site.value());
  writer.incarnation(message.incarnation);
  writer.epoch(message.observed_epoch);
  out = writer.buffer();
  return Status::OK;
}

Status decode_message(std::span<const std::uint8_t> payload, HeartbeatRequest& out) {
  internal::CanonicalReader reader(payload);
  Status status = internal::read_member_domain_key(reader, out.domain);
  if (!is_ok(status)) {
    return status;
  }
  status = internal::read_strong_id(reader, out.site);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.incarnation(out.incarnation);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.epoch(out.observed_epoch);
  if (!is_ok(status)) {
    return status;
  }
  return reader.finish();
}

MessageType message_type_of(const HeartbeatRequest&) { return MessageType::HEARTBEAT_REQUEST; }

std::string HeartbeatResponse::to_string() const {
  return std::string(site_fabric::to_string(status)) + " epoch=" + epoch.to_string() +
         " invalidated=" + std::to_string(invalidated.size());
}

Status encode_message(const HeartbeatResponse& message, std::vector<std::uint8_t>& out) {
  internal::CanonicalWriter writer;
  writer.u8(static_cast<std::uint8_t>(message.status));
  writer.epoch(message.epoch);
  writer.generation(message.site_generation);
  writer.u64(message.site_sequence);
  write_domain_list(writer, message.invalidated);
  internal::write_factors(writer, message.factors);
  out = writer.buffer();
  return Status::OK;
}

Status decode_message(std::span<const std::uint8_t> payload, HeartbeatResponse& out) {
  internal::CanonicalReader reader(payload);
  std::uint8_t status_code = 0;
  Status status = reader.u8(status_code);
  if (!is_ok(status)) {
    return status;
  }
  if (status_code > static_cast<std::uint8_t>(Status::NO_AUTHORITATIVE_SOURCE)) {
    return Status::MALFORMED;
  }
  out.status = static_cast<Status>(status_code);
  status = reader.epoch(out.epoch);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.generation(out.site_generation);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.u64(out.site_sequence);
  if (!is_ok(status)) {
    return status;
  }
  status = read_domain_list(reader, out.invalidated);
  if (!is_ok(status)) {
    return status;
  }
  status = decode_factors(reader, out.factors);
  if (!is_ok(status)) {
    return status;
  }
  return reader.finish();
}

MessageType message_type_of(const HeartbeatResponse&) { return MessageType::HEARTBEAT_RESPONSE; }

std::string InvalidateNotice::to_string() const {
  return site.value() + " sources=" + std::to_string(sources.size()) + " " + reason;
}

Status encode_message(const InvalidateNotice& message, std::vector<std::uint8_t>& out) {
  internal::CanonicalWriter writer;
  writer.text(message.site.value());
  writer.epoch(message.epoch);
  internal::write_source_set(writer, message.sources);
  writer.text(message.reason);
  out = writer.buffer();
  return Status::OK;
}

Status decode_message(std::span<const std::uint8_t> payload, InvalidateNotice& out) {
  internal::CanonicalReader reader(payload);
  Status status = internal::read_strong_id(reader, out.site);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.epoch(out.epoch);
  if (!is_ok(status)) {
    return status;
  }
  status = internal::read_source_set(reader, out.sources);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.text(out.reason, limits::kMaxTextBytes);
  if (!is_ok(status)) {
    return status;
  }
  return reader.finish();
}

MessageType message_type_of(const InvalidateNotice&) { return MessageType::INVALIDATE_NOTICE; }

std::string RevokeNotice::to_string() const {
  return domain.to_string() + " " + reason;
}

Status encode_message(const RevokeNotice& message, std::vector<std::uint8_t>& out) {
  internal::CanonicalWriter writer;
  writer.text(message.site.value());
  writer.epoch(message.epoch);
  internal::write_member_domain_key(writer, message.domain);
  writer.text(message.reason);
  out = writer.buffer();
  return Status::OK;
}

Status decode_message(std::span<const std::uint8_t> payload, RevokeNotice& out) {
  internal::CanonicalReader reader(payload);
  Status status = internal::read_strong_id(reader, out.site);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.epoch(out.epoch);
  if (!is_ok(status)) {
    return status;
  }
  status = internal::read_member_domain_key(reader, out.domain);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.text(out.reason, limits::kMaxTextBytes);
  if (!is_ok(status)) {
    return status;
  }
  return reader.finish();
}

MessageType message_type_of(const RevokeNotice&) { return MessageType::REVOKE_NOTICE; }

std::string ErrorResponse::to_string() const {
  std::string out = site_fabric::to_string(status);
  if (!detail.empty()) {
    out += " ";
    out += detail;
  }
  return out;
}

Status encode_message(const ErrorResponse& message, std::vector<std::uint8_t>& out) {
  internal::CanonicalWriter writer;
  writer.u8(static_cast<std::uint8_t>(message.status));
  writer.text(message.detail);
  internal::write_factors(writer, message.factors);
  out = writer.buffer();
  return Status::OK;
}

Status decode_message(std::span<const std::uint8_t> payload, ErrorResponse& out) {
  internal::CanonicalReader reader(payload);
  std::uint8_t status_code = 0;
  Status status = reader.u8(status_code);
  if (!is_ok(status)) {
    return status;
  }
  if (status_code > static_cast<std::uint8_t>(Status::NO_AUTHORITATIVE_SOURCE)) {
    return Status::MALFORMED;
  }
  out.status = static_cast<Status>(status_code);
  status = reader.text(out.detail, limits::kMaxTextBytes);
  if (!is_ok(status)) {
    return status;
  }
  status = decode_factors(reader, out.factors);
  if (!is_ok(status)) {
    return status;
  }
  return reader.finish();
}

MessageType message_type_of(const ErrorResponse&) { return MessageType::ERROR_RESPONSE; }

std::string ShutdownRequest::to_string() const {
  return site.value() + " " + reason;
}

Status encode_message(const ShutdownRequest& message, std::vector<std::uint8_t>& out) {
  internal::CanonicalWriter writer;
  writer.text(message.site.value());
  writer.incarnation(message.controller);
  writer.text(message.reason);
  out = writer.buffer();
  return Status::OK;
}

Status decode_message(std::span<const std::uint8_t> payload, ShutdownRequest& out) {
  internal::CanonicalReader reader(payload);
  Status status = internal::read_strong_id(reader, out.site);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.incarnation(out.controller);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.text(out.reason, limits::kMaxTextBytes);
  if (!is_ok(status)) {
    return status;
  }
  return reader.finish();
}

MessageType message_type_of(const ShutdownRequest&) { return MessageType::SHUTDOWN_REQUEST; }

std::string ShutdownResponse::to_string() const {
  return site_fabric::to_string(status);
}

Status encode_message(const ShutdownResponse& message, std::vector<std::uint8_t>& out) {
  internal::CanonicalWriter writer;
  writer.u8(static_cast<std::uint8_t>(message.status));
  internal::write_factors(writer, message.factors);
  out = writer.buffer();
  return Status::OK;
}

Status decode_message(std::span<const std::uint8_t> payload, ShutdownResponse& out) {
  internal::CanonicalReader reader(payload);
  std::uint8_t status_code = 0;
  Status status = reader.u8(status_code);
  if (!is_ok(status)) {
    return status;
  }
  if (status_code > static_cast<std::uint8_t>(Status::NO_AUTHORITATIVE_SOURCE)) {
    return Status::MALFORMED;
  }
  out.status = static_cast<Status>(status_code);
  status = decode_factors(reader, out.factors);
  if (!is_ok(status)) {
    return status;
  }
  return reader.finish();
}

MessageType message_type_of(const ShutdownResponse&) { return MessageType::SHUTDOWN_RESPONSE; }

std::string PingMessage::to_string() const { return "nonce=" + std::to_string(nonce); }

Status encode_message(const PingMessage& message, std::vector<std::uint8_t>& out) {
  internal::CanonicalWriter writer;
  writer.u64(message.nonce);
  out = writer.buffer();
  return Status::OK;
}

Status decode_message(std::span<const std::uint8_t> payload, PingMessage& out) {
  internal::CanonicalReader reader(payload);
  const Status status = reader.u64(out.nonce);
  if (!is_ok(status)) {
    return status;
  }
  return reader.finish();
}

MessageType message_type_of(const PingMessage&) { return MessageType::PING; }

}  // namespace site_fabric
