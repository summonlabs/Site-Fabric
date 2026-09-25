// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The site controller.
//
// The controller is the single writer of site snapshots. It holds the member
// publications, fences every offer against the authority registry, composes on
// demand, and publishes the result to a bounded snapshot chain. It is the only
// component that decides what the site currently is.
//
// Concurrency contract, stated here because it is part of the interface:
//
//   * The registry lock is a single std::mutex. It is taken only for the
//     duration of a map update, never across a composition, never across a
//     socket operation, and never while another lock is held.
//   * Composition runs on a copy of the inputs. No lock is held while the
//     composer runs, so a slow composition cannot block a publication and a
//     publication cannot deadlock against a composition.
//   * The snapshot chain has its own mutex, taken after the registry lock has
//     been released. There is no path that takes the registry lock while
//     holding the chain lock.
//   * Worker threads are joined only after the listener is closed and the stop
//     flag is set, and no worker is joined while it holds state the joiner
//     needs.
//   * No callback supplied by a caller is invoked under a lock.

#ifndef SITE_FABRIC_CONTROLLER_HPP
#define SITE_FABRIC_CONTROLLER_HPP

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "site_fabric/authority.hpp"
#include "site_fabric/persistence.hpp"
#include "site_fabric/protocol.hpp"
#include "site_fabric/version.hpp"

namespace site_fabric {

/// A monotonic millisecond clock. The default reads the system clock; tests
/// substitute a deterministic one.
using Clock = std::function<std::int64_t()>;

[[nodiscard]] std::int64_t system_now_ms();

struct ControllerConfig {
  SiteId site;
  SiteExpectation expectation;
  ControllerId id;

  std::string bind_host = "127.0.0.1";
  /// Zero asks the operating system for an ephemeral port, which is what the
  /// tests use so that no test can collide with another.
  std::uint16_t bind_port = 0;

  Incarnation incarnation;
  SchemaVersion schema{kModelSchemaMajor, kModelSchemaMinor};

  std::int64_t lease_ttl_ms = 30'000;
  /// The maximum number of member sessions served at once. Each session is a
  /// connection and a handler thread, so this is a real bound on both. A peer
  /// above the bound is refused rather than queued indefinitely.
  std::size_t max_connections = 64;
  std::size_t accept_backlog = 32;
  std::int64_t io_timeout_ms = 10'000;

  bool retain_declarations = true;
  bool persistent = false;
  StoreConfig store;
  std::uint32_t snapshot_history = limits::kDefaultSnapshotHistory;

  /// Recompose automatically after every accepted publication.
  bool compose_on_publish = true;

  Clock clock;
};

/// Counters, readable at any time without blocking a worker.
struct ControllerStats {
  std::uint64_t connections_accepted = 0;
  std::uint64_t connections_refused = 0;
  std::uint64_t frames_received = 0;
  std::uint64_t frames_rejected = 0;
  std::uint64_t publications_accepted = 0;
  std::uint64_t publications_refused = 0;
  std::uint64_t publications_replayed = 0;
  std::uint64_t retirements_accepted = 0;
  std::uint64_t compositions = 0;
  std::uint64_t snapshots_published = 0;
  std::uint64_t snapshots_refused = 0;
  std::uint64_t hello_accepted = 0;
  std::uint64_t hello_refused = 0;
  std::uint64_t heartbeats = 0;
  std::uint64_t recoveries = 0;
  std::uint64_t recovery_rejections = 0;
};

/// The site controller.
///
/// Create with create(), then start(). Every entry point is safe to call from
/// any thread, including concurrently with start() and stop(), with the single
/// exception of destroy, which must not race another call.
class SiteController {
 public:
  ~SiteController();

  SiteController(const SiteController&) = delete;
  SiteController& operator=(const SiteController&) = delete;

  /// Builds a controller. Does not bind a socket, open a store, or take
  /// authority; those happen in start().
  [[nodiscard]] static Status create(const ControllerConfig& config,
                                     std::unique_ptr<SiteController>& out);

  /// Takes site authority, recovers the store if configured, binds the
  /// listener, and launches the workers.
  Status start();

  /// Stops the workers, closes the listener, closes the store and relinquishes
  /// the lease. Idempotent.
  Status stop();

  /// Runs until the flag is set or stop() is called. Returns OK on a clean
  /// stop and STOPPED when the listener was already closed underneath it.
  Status serve(std::atomic<bool>& stop_flag);

  // --- State access --------------------------------------------------------

  /// Offers one member publication. Applies every fence before recording.
  Status publish(const MemberId& publisher, const Incarnation& incarnation,
                 const MemberDomainDeclaration& declaration, const PublishAttempt& attempt,
                 PublishResponse& out);

  /// Offers one retirement.
  Status retire(const RetireRequest& request, RetireResponse& out);

  /// Recomposes from the current registry and publishes a snapshot.
  Status recompose(SnapshotPublishResult& out);

  /// Composes without publishing. Used by the inspection tool.
  Status compose_now(ComposedSite& out) const;

  [[nodiscard]] Status current_snapshot(SiteSnapshot& out) const;
  [[nodiscard]] Status snapshot_at(std::uint64_t sequence, SiteSnapshot& out) const;

  [[nodiscard]] Status snapshot_sequences(std::vector<std::uint64_t>& out) const;

  // --- Introspection -------------------------------------------------------

  [[nodiscard]] const SiteId& site() const noexcept;
  [[nodiscard]] Epoch epoch() const;
  [[nodiscard]] Generation generation() const;
  [[nodiscard]] Incarnation incarnation() const;
  [[nodiscard]] std::uint16_t port() const;
  [[nodiscard]] std::string endpoint() const;
  [[nodiscard]] bool running() const noexcept;
  [[nodiscard]] ControllerStats stats() const;
  [[nodiscard]] AuthorityRegistry& authority();
  [[nodiscard]] const AuthorityRegistry& authority() const;
  [[nodiscard]] const SiteExpectation& expectation() const;
  [[nodiscard]] const ControllerConfig& config() const;
  [[nodiscard]] std::string status_text() const;
  [[nodiscard]] const StoreRecoveryReport& recovery() const;

  /// Marks a member domain's authority revoked and recomposes.
  Status revoke_member(const MemberDomainKey& domain, std::string reason);

  /// Returns the domains whose accepted publication is newer than the given
  /// acceptance mark, which is how a heartbeat tells a member what it missed.
  [[nodiscard]] Status changed_since(std::uint64_t since_sequence,
                                     std::vector<MemberDomainKey>& out) const;

 private:
  explicit SiteController(const ControllerConfig& config);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace site_fabric

#endif  // SITE_FABRIC_CONTROLLER_HPP
