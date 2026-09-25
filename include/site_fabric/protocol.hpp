// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The wire protocol.
//
// Framing is length-prefixed and self-checking. A frame carries a magic, the
// protocol version, the message type, a payload length and a digest over the
// payload. A reader validates every one of those before it allocates: the
// length is checked against the configured maximum first, then the digest,
// then the payload is decoded. A frame that fails any check is refused with a
// distinguishing status and the connection is closed, because a stream whose
// framing is no longer trusted cannot be resynchronised by guessing.

#ifndef SITE_FABRIC_PROTOCOL_HPP
#define SITE_FABRIC_PROTOCOL_HPP

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "site_fabric/member.hpp"
#include "site_fabric/snapshot.hpp"
#include "site_fabric/version.hpp"

namespace site_fabric {

/// Magic at the start of every frame: 'S','F','A','B'.
inline constexpr std::uint32_t kFrameMagic = 0x53464142U;

/// Bytes of frame header preceding the payload.
inline constexpr std::size_t kFrameHeaderBytes = 48;

enum class MessageType : std::uint16_t {
  UNKNOWN = 0,
  HELLO_REQUEST = 1,
  HELLO_RESPONSE = 2,
  PUBLISH_REQUEST = 3,
  PUBLISH_RESPONSE = 4,
  RETIRE_REQUEST = 5,
  RETIRE_RESPONSE = 6,
  FETCH_SITE_REQUEST = 7,
  FETCH_SITE_RESPONSE = 8,
  HEARTBEAT_REQUEST = 9,
  HEARTBEAT_RESPONSE = 10,
  INVALIDATE_NOTICE = 11,
  REVOKE_NOTICE = 12,
  ERROR_RESPONSE = 13,
  SHUTDOWN_REQUEST = 14,
  SHUTDOWN_RESPONSE = 15,
  PING = 16,
  PONG = 17,
};

[[nodiscard]] const char* to_string(MessageType type);
[[nodiscard]] bool is_known(MessageType type) noexcept;

/// The decoded frame header.
struct FrameHeader {
  std::uint16_t protocol_version = 0;
  MessageType type = MessageType::UNKNOWN;
  std::uint32_t flags = 0;
  std::uint32_t payload_bytes = 0;
  Digest payload_digest;

  [[nodiscard]] std::string to_string() const;
};

/// Encodes a frame: header, then payload.
///
/// Returns Status::LIMIT_EXCEEDED when the payload exceeds limits::kMaxFrameBytes
/// and Status::CAPACITY_OVERFLOW when the total would not fit in the header's 32-bit
/// length field.
Status encode_frame(MessageType type, std::span<const std::uint8_t> payload,
                    std::vector<std::uint8_t>& out, std::uint32_t flags = 0);

/// Decodes a frame header from exactly kFrameHeaderBytes bytes. Validates the
/// magic, the protocol version, the length bound and the header digest.
Status decode_frame_header(std::span<const std::uint8_t> header_bytes, FrameHeader& out);

/// Verifies a payload against a decoded header.
Status verify_payload(const FrameHeader& header, std::span<const std::uint8_t> payload);

// --- Messages --------------------------------------------------------------

/// A publisher introducing itself.
struct HelloRequest {
  MemberDomainKey domain;
  SiteId site;
  Incarnation incarnation;
  MemberId publisher;
  Epoch observed_epoch;
  SchemaVersion schema{kModelSchemaMajor, kModelSchemaMinor};
  /// The generation the publisher intends to start from; zero means "whatever
  /// the controller has".
  Generation resume_from;

  [[nodiscard]] std::string to_string() const;
};

struct HelloResponse {
  Status status = Status::UNKNOWN;
  SiteId site;
  Epoch epoch;
  Incarnation controller;
  /// The sequence the controller assigned to this incarnation. Zero when the
  /// request was refused.
  std::uint64_t assigned_sequence = 0;
  Generation current_generation;
  Factors factors;

  [[nodiscard]] std::string to_string() const;
};

struct PublishRequest {
  MemberDomainDeclaration declaration;
  PublishAttempt attempt;

  [[nodiscard]] std::string to_string() const;
};

struct PublishResponse {
  Status status = Status::UNKNOWN;
  SiteId site;
  Epoch epoch;
  Generation site_generation;
  std::uint64_t acceptance_sequence = 0;
  bool recomposed = false;
  Factors factors;

  [[nodiscard]] std::string to_string() const;
};

struct RetireRequestMessage {
  RetireRequest request;

  [[nodiscard]] std::string to_string() const;
};

struct RetireResponse {
  Status status = Status::UNKNOWN;
  SiteId site;
  Epoch epoch;
  std::uint64_t acceptance_sequence = 0;
  Factors factors;

  [[nodiscard]] std::string to_string() const;
};

struct FetchSiteRequest {
  SiteId site;
  /// Zero asks for the current snapshot.
  std::uint64_t sequence = 0;
  bool include_state = true;

  [[nodiscard]] std::string to_string() const;
};

struct FetchSiteResponse {
  Status status = Status::UNKNOWN;
  bool has_snapshot = false;
  SiteSnapshot snapshot;
  Factors factors;

  [[nodiscard]] std::string to_string() const;
};

struct HeartbeatRequest {
  MemberDomainKey domain;
  SiteId site;
  Incarnation incarnation;
  Epoch observed_epoch;
  /// The acceptance sequence the caller last observed. Zero asks for
  /// everything the controller currently holds.
  std::uint64_t since_sequence = 0;

  [[nodiscard]] std::string to_string() const;
};

struct HeartbeatResponse {
  Status status = Status::UNKNOWN;
  Epoch epoch;
  Generation site_generation;
  std::uint64_t site_sequence = 0;
  /// Domains the controller believes changed since the caller's last
  /// generation.
  std::vector<MemberDomainKey> invalidated;
  Factors factors;

  [[nodiscard]] std::string to_string() const;
};

struct InvalidateNotice {
  SiteId site;
  Epoch epoch;
  SourceSet sources;
  std::string reason;

  [[nodiscard]] std::string to_string() const;
};

struct RevokeNotice {
  SiteId site;
  Epoch epoch;
  MemberDomainKey domain;
  std::string reason;

  [[nodiscard]] std::string to_string() const;
};

struct ErrorResponse {
  Status status = Status::UNKNOWN;
  std::string detail;
  Factors factors;

  [[nodiscard]] std::string to_string() const;
};

struct ShutdownRequest {
  SiteId site;
  Incarnation controller;
  std::string reason;

  [[nodiscard]] std::string to_string() const;
};

struct ShutdownResponse {
  Status status = Status::UNKNOWN;
  Factors factors;

  [[nodiscard]] std::string to_string() const;
};

struct PingMessage {
  std::uint64_t nonce = 0;

  [[nodiscard]] std::string to_string() const;
};

// --- Codecs ----------------------------------------------------------------

Status encode_message(const HelloRequest& message, std::vector<std::uint8_t>& out);
Status decode_message(std::span<const std::uint8_t> payload, HelloRequest& out);

Status encode_message(const HelloResponse& message, std::vector<std::uint8_t>& out);
Status decode_message(std::span<const std::uint8_t> payload, HelloResponse& out);

Status encode_message(const PublishRequest& message, std::vector<std::uint8_t>& out);
Status decode_message(std::span<const std::uint8_t> payload, PublishRequest& out);

Status encode_message(const PublishResponse& message, std::vector<std::uint8_t>& out);
Status decode_message(std::span<const std::uint8_t> payload, PublishResponse& out);

Status encode_message(const RetireRequestMessage& message, std::vector<std::uint8_t>& out);
Status decode_message(std::span<const std::uint8_t> payload, RetireRequestMessage& out);

Status encode_message(const RetireResponse& message, std::vector<std::uint8_t>& out);
Status decode_message(std::span<const std::uint8_t> payload, RetireResponse& out);

Status encode_message(const FetchSiteRequest& message, std::vector<std::uint8_t>& out);
Status decode_message(std::span<const std::uint8_t> payload, FetchSiteRequest& out);

Status encode_message(const FetchSiteResponse& message, std::vector<std::uint8_t>& out);
Status decode_message(std::span<const std::uint8_t> payload, FetchSiteResponse& out);

Status encode_message(const HeartbeatRequest& message, std::vector<std::uint8_t>& out);
Status decode_message(std::span<const std::uint8_t> payload, HeartbeatRequest& out);

Status encode_message(const HeartbeatResponse& message, std::vector<std::uint8_t>& out);
Status decode_message(std::span<const std::uint8_t> payload, HeartbeatResponse& out);

Status encode_message(const InvalidateNotice& message, std::vector<std::uint8_t>& out);
Status decode_message(std::span<const std::uint8_t> payload, InvalidateNotice& out);

Status encode_message(const RevokeNotice& message, std::vector<std::uint8_t>& out);
Status decode_message(std::span<const std::uint8_t> payload, RevokeNotice& out);

Status encode_message(const ErrorResponse& message, std::vector<std::uint8_t>& out);
Status decode_message(std::span<const std::uint8_t> payload, ErrorResponse& out);

Status encode_message(const ShutdownRequest& message, std::vector<std::uint8_t>& out);
Status decode_message(std::span<const std::uint8_t> payload, ShutdownRequest& out);

Status encode_message(const ShutdownResponse& message, std::vector<std::uint8_t>& out);
Status decode_message(std::span<const std::uint8_t> payload, ShutdownResponse& out);

Status encode_message(const PingMessage& message, std::vector<std::uint8_t>& out);
Status decode_message(std::span<const std::uint8_t> payload, PingMessage& out);

/// The message type a codec handles, for dispatch tables.
[[nodiscard]] MessageType message_type_of(const HelloRequest&);
[[nodiscard]] MessageType message_type_of(const HelloResponse&);
[[nodiscard]] MessageType message_type_of(const PublishRequest&);
[[nodiscard]] MessageType message_type_of(const PublishResponse&);
[[nodiscard]] MessageType message_type_of(const RetireRequestMessage&);
[[nodiscard]] MessageType message_type_of(const RetireResponse&);
[[nodiscard]] MessageType message_type_of(const FetchSiteRequest&);
[[nodiscard]] MessageType message_type_of(const FetchSiteResponse&);
[[nodiscard]] MessageType message_type_of(const HeartbeatRequest&);
[[nodiscard]] MessageType message_type_of(const HeartbeatResponse&);
[[nodiscard]] MessageType message_type_of(const InvalidateNotice&);
[[nodiscard]] MessageType message_type_of(const RevokeNotice&);
[[nodiscard]] MessageType message_type_of(const ErrorResponse&);
[[nodiscard]] MessageType message_type_of(const ShutdownRequest&);
[[nodiscard]] MessageType message_type_of(const ShutdownResponse&);
[[nodiscard]] MessageType message_type_of(const PingMessage&);

}  // namespace site_fabric

#endif  // SITE_FABRIC_PROTOCOL_HPP
