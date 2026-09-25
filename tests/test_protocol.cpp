// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdint>
#include <string>
#include <vector>

#include "site_fabric/limits.hpp"
#include "site_fabric/protocol.hpp"
#include "site_fabric/site_fabric.hpp"
#include "site_fabric/synthetic.hpp"
#include "test_support.hpp"

using namespace site_fabric;

namespace {

template <class Message>
bool round_trip(const Message& message, Message& decoded) {
  std::vector<std::uint8_t> payload;
  if (!is_ok(encode_message(message, payload))) {
    return false;
  }
  if (!is_ok(decode_message(payload, decoded))) {
    return false;
  }
  return true;
}

std::vector<std::uint8_t> frame_of(MessageType type,
                                   const std::vector<std::uint8_t>& payload) {
  std::vector<std::uint8_t> frame;
  (void)encode_frame(type, payload, frame);
  return frame;
}

}  // namespace

SF_TEST(protocol, frame_round_trip) {
  const std::vector<std::uint8_t> payload = {1, 2, 3, 4, 5};
  std::vector<std::uint8_t> frame;
  SF_REQUIRE(is_ok(encode_frame(MessageType::PING, payload, frame)));
  SF_CHECK_EQ(kFrameHeaderBytes + payload.size(), frame.size());
  SF_CHECK_EQ(std::uint32_t(kFrameMagic), static_cast<std::uint32_t>(frame[0]) |
                                              (static_cast<std::uint32_t>(frame[1]) << 8U) |
                                              (static_cast<std::uint32_t>(frame[2]) << 16U) |
                                              (static_cast<std::uint32_t>(frame[3]) << 24U));
  FrameHeader header;
  SF_REQUIRE(is_ok(decode_frame_header(
      std::span<const std::uint8_t>(frame.data(), kFrameHeaderBytes), header)));
  SF_CHECK_EQ(MessageType::PING, header.type);
  SF_CHECK_EQ(std::uint16_t(kWireProtocolVersion), header.protocol_version);
  SF_CHECK_EQ(std::uint32_t(payload.size()), header.payload_bytes);
  SF_REQUIRE(is_ok(verify_payload(
      header, std::span<const std::uint8_t>(frame.data() + kFrameHeaderBytes,
                                            payload.size()))));
}

SF_TEST(protocol, frame_header_refusals) {
  const std::vector<std::uint8_t> payload = {9, 9, 9};
  std::vector<std::uint8_t> frame = frame_of(MessageType::PING, payload);

  FrameHeader header;
  SF_CHECK_EQ(Status::TRUNCATED,
              decode_frame_header(std::span<const std::uint8_t>(frame.data(), 10), header));

  std::vector<std::uint8_t> bad_magic = frame;
  bad_magic[0] ^= 0xFFU;
  SF_CHECK_EQ(Status::MALFORMED,
              decode_frame_header(
                  std::span<const std::uint8_t>(bad_magic.data(), kFrameHeaderBytes), header));

  std::vector<std::uint8_t> bad_version = frame;
  bad_version[4] = 0x7FU;
  SF_CHECK_EQ(Status::UNSUPPORTED_FORMAT,
              decode_frame_header(
                  std::span<const std::uint8_t>(bad_version.data(), kFrameHeaderBytes), header));

  std::vector<std::uint8_t> bad_type = frame;
  bad_type[6] = 0xFFU;
  bad_type[7] = 0x00U;
  SF_CHECK_EQ(Status::UNSUPPORTED,
              decode_frame_header(
                  std::span<const std::uint8_t>(bad_type.data(), kFrameHeaderBytes), header));

  std::vector<std::uint8_t> too_long = frame;
  const std::uint32_t huge = 0xFFFFFFF0U;
  for (int index = 0; index < 4; ++index) {
    too_long[12 + static_cast<std::size_t>(index)] =
        static_cast<std::uint8_t>((huge >> (8 * index)) & 0xFFU);
  }
  SF_CHECK_EQ(Status::LIMIT_EXCEEDED,
              decode_frame_header(
                  std::span<const std::uint8_t>(too_long.data(), kFrameHeaderBytes), header));
}

SF_TEST(protocol, payload_verification) {
  const std::vector<std::uint8_t> payload = {1, 2, 3};
  std::vector<std::uint8_t> frame = frame_of(MessageType::PING, payload);
  FrameHeader header;
  SF_REQUIRE(is_ok(decode_frame_header(
      std::span<const std::uint8_t>(frame.data(), kFrameHeaderBytes), header)));

  SF_CHECK_EQ(Status::TRUNCATED,
              verify_payload(header, std::span<const std::uint8_t>(frame.data() + kFrameHeaderBytes,
                                                                   2)));
  std::vector<std::uint8_t> tampered(payload);
  tampered[1] ^= 0xFFU;
  SF_CHECK_EQ(Status::INTEGRITY_FAILURE,
              verify_payload(header, std::span<const std::uint8_t>(tampered.data(),
                                                                   tampered.size())));
}

SF_TEST(protocol, oversized_frame_is_refused) {
  std::vector<std::uint8_t> payload(limits::kMaxFrameBytes + 1, 0);
  std::vector<std::uint8_t> frame;
  SF_CHECK_EQ(Status::LIMIT_EXCEEDED, encode_frame(MessageType::PING, payload, frame));
  SF_CHECK(frame.empty());
}

SF_TEST(protocol, hello_round_trip) {
  HelloRequest request;
  request.domain = MemberDomainKey::rack("rack-a");
  request.site = SiteId::unchecked("site-alpha");
  request.incarnation = Incarnation(3, BootNonce(std::string("boot")));
  request.publisher = MemberId::unchecked("publisher-1");
  request.observed_epoch = Epoch(2);
  request.resume_from = Generation(4);

  HelloRequest decoded;
  SF_REQUIRE(round_trip(request, decoded));
  SF_CHECK(decoded.domain == request.domain);
  SF_CHECK(decoded.incarnation.sequence == request.incarnation.sequence);
  SF_CHECK_EQ(std::string("boot"), decoded.incarnation.boot.value);
  SF_CHECK(decoded.publisher == request.publisher);
  SF_CHECK_EQ(Epoch(2), decoded.observed_epoch);
  SF_CHECK_EQ(Generation(4), decoded.resume_from);

  HelloResponse response;
  response.status = Status::OK;
  response.site = request.site;
  response.epoch = Epoch(5);
  response.controller = Incarnation(1, BootNonce(std::string("ctl")));
  response.assigned_sequence = 9;
  response.current_generation = Generation(7);
  response.factors.add("a", "b");
  HelloResponse decoded_response;
  SF_REQUIRE(round_trip(response, decoded_response));
  SF_CHECK_EQ(Status::OK, decoded_response.status);
  SF_CHECK_EQ(std::uint64_t(9), decoded_response.assigned_sequence);
  SF_CHECK_EQ(Generation(7), decoded_response.current_generation);
  SF_CHECK_EQ(std::size_t(1), decoded_response.factors.size());
}

SF_TEST(protocol, declaration_publish_round_trip) {
  MemberDomainDeclaration declaration =
      sftest::simple_rack_declaration("rack-a", "r-a", 4096, 1000000, 60000);
  SF_REQUIRE(is_ok(declaration.seal()));
  PublishRequest request;
  request.declaration = declaration;
  request.attempt = PublishAttempt{3, 2};

  PublishRequest decoded;
  SF_REQUIRE(round_trip(request, decoded));
  SF_CHECK(decoded.declaration.digest == declaration.digest);
  SF_CHECK(decoded.declaration.compute_digest() == declaration.digest);
  SF_CHECK_EQ(std::uint64_t(3), decoded.attempt.sequence);
  SF_CHECK_EQ(std::uint32_t(2), decoded.attempt.attempt);

  PublishResponse response;
  response.status = Status::ACCEPTED;
  response.site = SiteId::unchecked("site-alpha");
  response.epoch = Epoch(1);
  response.site_generation = Generation(12);
  response.acceptance_sequence = 44;
  response.recomposed = true;
  PublishResponse decoded_response;
  SF_REQUIRE(round_trip(response, decoded_response));
  SF_CHECK_EQ(Status::ACCEPTED, decoded_response.status);
  SF_CHECK_EQ(std::uint64_t(44), decoded_response.acceptance_sequence);
  SF_CHECK(decoded_response.recomposed);
}

SF_TEST(protocol, snapshot_fetch_round_trip) {
  SyntheticConfig config;
  config.clusters = 1;
  config.pods_per_cluster = 1;
  config.racks_per_pod = 1;
  config.shared_links = 1;
  config.gateways = 1;
  config.failure_domains = 1;
  config.maintenance_zones = 0;
  config.obligations = 0;
  SyntheticSite generated;
  SF_REQUIRE(is_ok(generate_synthetic_site(config, generated)));
  ComposedSite composed;
  SiteComposer composer;
  SF_REQUIRE(is_ok(composer.compose(generated.input, composed)));

  SiteSnapshot snapshot;
  snapshot.site = composed.site;
  snapshot.epoch = composed.epoch;
  snapshot.generation = composed.generation;
  snapshot.incarnation = composed.incarnation;
  snapshot.sequence = 3;
  snapshot.published_at_ms = 1000000;
  snapshot.lifecycle = composed.lifecycle;
  snapshot.status = composed.status;
  snapshot.complete = true;
  snapshot.state = composed;
  SF_REQUIRE(is_ok(snapshot.recompute_digest()));

  FetchSiteResponse response;
  response.status = Status::OK;
  response.has_snapshot = true;
  response.snapshot = snapshot;

  FetchSiteResponse decoded;
  SF_REQUIRE(round_trip(response, decoded));
  SF_CHECK(decoded.has_snapshot);
  SF_CHECK_EQ(std::uint64_t(3), decoded.snapshot.sequence);
  SF_CHECK(decoded.snapshot.site_digest == snapshot.site_digest);
  SF_CHECK(decoded.snapshot.state.compute_digest() == composed.compute_digest());
  SF_CHECK_EQ(composed.members.size(), decoded.snapshot.state.members.size());
  SF_CHECK_EQ(composed.ownership.size(), decoded.snapshot.state.ownership.size());
  SF_CHECK_EQ(composed.capacity.entries.size(), decoded.snapshot.state.capacity.entries.size());
  SF_CHECK_EQ(composed.decisions.size(), decoded.snapshot.state.decisions.size());
  SF_CHECK(decoded.snapshot.state.capacity.total == composed.capacity.total);

  FetchSiteRequest request;
  request.site = composed.site;
  request.sequence = 3;
  request.include_state = false;
  FetchSiteRequest decoded_request;
  SF_REQUIRE(round_trip(request, decoded_request));
  SF_CHECK_EQ(std::uint64_t(3), decoded_request.sequence);
  SF_CHECK(!decoded_request.include_state);
}

SF_TEST(protocol, remaining_messages_round_trip) {
  InvalidateNotice notice;
  notice.site = SiteId::unchecked("site-alpha");
  notice.epoch = Epoch(2);
  notice.sources.insert(SourceRef(MemberDomainKey::rack("rack-a"), Generation(1),
                                  internal::digest_text("d")));
  notice.reason = "member changed";
  InvalidateNotice decoded_notice;
  SF_REQUIRE(round_trip(notice, decoded_notice));
  SF_CHECK_EQ(std::size_t(1), decoded_notice.sources.size());
  SF_CHECK_EQ(std::string("member changed"), decoded_notice.reason);

  RevokeNotice revoke;
  revoke.site = SiteId::unchecked("site-alpha");
  revoke.epoch = Epoch(1);
  revoke.domain = MemberDomainKey::cluster("cluster-1");
  revoke.reason = "operator";
  RevokeNotice decoded_revoke;
  SF_REQUIRE(round_trip(revoke, decoded_revoke));
  SF_CHECK(decoded_revoke.domain == revoke.domain);

  ErrorResponse error;
  error.status = Status::MALFORMED;
  error.detail = "bad frame";
  ErrorResponse decoded_error;
  SF_REQUIRE(round_trip(error, decoded_error));
  SF_CHECK_EQ(Status::MALFORMED, decoded_error.status);

  ShutdownRequest shutdown;
  shutdown.site = SiteId::unchecked("site-alpha");
  shutdown.controller = Incarnation(1, BootNonce(std::string("x")));
  shutdown.reason = "operator";
  ShutdownRequest decoded_shutdown;
  SF_REQUIRE(round_trip(shutdown, decoded_shutdown));
  SF_CHECK_EQ(std::string("operator"), decoded_shutdown.reason);

  ShutdownResponse shutdown_response;
  shutdown_response.status = Status::OK;
  ShutdownResponse decoded_shutdown_response;
  SF_REQUIRE(round_trip(shutdown_response, decoded_shutdown_response));
  SF_CHECK_EQ(Status::OK, decoded_shutdown_response.status);

  PingMessage ping;
  ping.nonce = 0xDEADBEEFULL;
  PingMessage decoded_ping;
  SF_REQUIRE(round_trip(ping, decoded_ping));
  SF_CHECK_EQ(std::uint64_t(0xDEADBEEFULL), decoded_ping.nonce);
}

SF_TEST(protocol, unknown_status_code_is_malformed) {
  PublishResponse response;
  response.status = Status::OK;
  std::vector<std::uint8_t> payload;
  SF_REQUIRE(is_ok(encode_message(response, payload)));
  payload[0] = 0xF0U;
  PublishResponse decoded;
  SF_CHECK_EQ(Status::MALFORMED, decode_message(payload, decoded));
}

SF_TEST(protocol, truncated_messages_are_refused_at_every_length) {
  HelloRequest request;
  request.domain = MemberDomainKey::rack("rack-a");
  request.site = SiteId::unchecked("site-alpha");
  request.incarnation = Incarnation(1, BootNonce(std::string("boot")));
  request.publisher = MemberId::unchecked("publisher");
  std::vector<std::uint8_t> payload;
  SF_REQUIRE(is_ok(encode_message(request, payload)));
  for (std::size_t length = 0; length < payload.size(); ++length) {
    HelloRequest decoded;
    const Status status =
        decode_message(std::span<const std::uint8_t>(payload.data(), length), decoded);
    SF_CHECK(!is_ok(status));
  }
}
