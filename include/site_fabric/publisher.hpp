// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The member-domain publisher.
//
// A publisher is a member domain's agent. It owns exactly one declaration, it
// seals it, and it offers it to the controller under an incarnation it was
// issued. It has no authority of its own: a publisher that has not completed a
// hello cannot publish, and a publisher whose incarnation has been retired
// cannot publish again even if it kept its token.

#ifndef SITE_FABRIC_PUBLISHER_HPP
#define SITE_FABRIC_PUBLISHER_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "site_fabric/protocol.hpp"
#include "site_fabric/version.hpp"

namespace site_fabric {

struct PublisherConfig {
  std::string host = "127.0.0.1";
  std::uint16_t port = 0;
  SiteId site;
  MemberDomainKey domain;
  MemberId publisher;
  Incarnation incarnation;
  SchemaVersion schema{kModelSchemaMajor, kModelSchemaMinor};
  std::int64_t io_timeout_ms = 10'000;
  std::int64_t connect_timeout_ms = 5'000;
};

/// The publisher's view of its own session.
struct PublisherSession {
  bool established = false;
  Epoch epoch;
  Incarnation controller;
  std::uint64_t assigned_sequence = 0;
  Generation controller_generation;
  Status status = Status::UNKNOWN;
  Factors factors;

  [[nodiscard]] std::string to_string() const;
};

/// A member-domain client over the framed transport.
///
/// Not internally threaded: the caller decides when to publish and when to
/// heartbeat. Every method is safe to call from one thread at a time and
/// reports BUSY rather than interleaving two requests on one connection.
class MemberPublisher {
 public:
  ~MemberPublisher();

  MemberPublisher(const MemberPublisher&) = delete;
  MemberPublisher& operator=(const MemberPublisher&) = delete;

  [[nodiscard]] static Status create(const PublisherConfig& config,
                                     std::unique_ptr<MemberPublisher>& out);

  /// Opens the socket and performs the hello handshake.
  Status connect();

  /// Performs the hello handshake on an already-open connection.
  Status handshake(PublisherSession& out);

  Status publish(const MemberDomainDeclaration& declaration, const PublishAttempt& attempt,
                 PublishResponse& out);

  Status retire(const RetireRequest& request, RetireResponse& out);

  Status heartbeat(HeartbeatResponse& out);

  Status fetch_site(std::uint64_t sequence, FetchSiteResponse& out);

  Status ping(std::uint64_t nonce, std::uint64_t& echoed_nonce);

  Status close();

  [[nodiscard]] bool connected() const noexcept;
  [[nodiscard]] const PublisherSession& session() const noexcept;
  [[nodiscard]] const PublisherConfig& config() const noexcept;

 private:
  explicit MemberPublisher(const PublisherConfig& config);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace site_fabric

#endif  // SITE_FABRIC_PUBLISHER_HPP
