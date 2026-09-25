// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Site snapshots.
//
// A snapshot is the immutable publication of one composed site. Snapshots are
// numbered, digest-verified and totally ordered, and a snapshot may only be
// published by a controller that still holds authority for the epoch it
// claims. The chain refuses a token whose epoch, incarnation or generation has
// been superseded, which is what makes a stale controller unable to publish:
// the refusal happens inside the chain, not in the caller's conscience.

#ifndef SITE_FABRIC_SNAPSHOT_HPP
#define SITE_FABRIC_SNAPSHOT_HPP

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "site_fabric/authority.hpp"
#include "site_fabric/composition.hpp"

namespace site_fabric {

/// An immutable publication of a composed site.
struct SiteSnapshot {
  SiteId site;
  Epoch epoch;
  Generation generation;
  Incarnation incarnation;
  /// Monotonic publication number, assigned by the chain.
  std::uint64_t sequence = 0;
  /// The composed site's own digest.
  Digest site_digest;
  /// Digest of this envelope including the sequence.
  Digest snapshot_digest;
  std::int64_t published_at_ms = 0;
  SiteLifecycle lifecycle = SiteLifecycle::UNKNOWN;
  Status status = Status::UNKNOWN;
  bool complete = false;
  /// The exact member sources this snapshot rests on.
  SourceSet sources;
  Factors factors;
  /// The full composed state.
  ComposedSite state;

  /// Recomputes the envelope digest. Returns Status::INVALID when the site
  /// digest does not match the embedded state, because publishing a snapshot
  /// whose envelope disagrees with its content is exactly the failure the
  /// digest exists to catch.
  [[nodiscard]] Status recompute_digest();

  [[nodiscard]] std::string to_string() const;
};

/// The result of offering a snapshot to the chain.
struct SnapshotPublishResult {
  Status status = Status::UNKNOWN;
  std::uint64_t sequence = 0;
  Factors factors;

  [[nodiscard]] bool accepted() const noexcept {
    return status == Status::OK || status == Status::PUBLISHED ||
           status == Status::ALREADY_EXISTS;
  }
  [[nodiscard]] std::string to_string() const;
};

/// A bounded, ordered chain of snapshots.
///
/// Thread-safe: one mutex guards the whole chain, no callback runs under it,
/// and no operation requires a second lock.
class SnapshotChain {
 public:
  explicit SnapshotChain(std::uint32_t history_limit = 0);
  ~SnapshotChain();

  SnapshotChain(const SnapshotChain&) = delete;
  SnapshotChain& operator=(const SnapshotChain&) = delete;

  /// Validates the token, fences it against the incumbent, and appends.
  ///
  /// Idempotent republication of the current snapshot by the current
  /// incarnation at the same generation returns Status::ALREADY_EXISTS without
  /// advancing the sequence.
  Status publish(const SiteSnapshot& snapshot, const AuthorityToken& token,
                 std::int64_t now_ms, SnapshotPublishResult& out);

  /// Installs a snapshot recovered from storage, bypassing the token checks but
  /// not the sequence or digest checks. Returns INTEGRITY_FAILURE for a
  /// snapshot whose digest does not match its content.
  Status restore(const SiteSnapshot& snapshot);

  [[nodiscard]] bool current(SiteSnapshot& out) const;
  [[nodiscard]] bool at_sequence(std::uint64_t sequence, SiteSnapshot& out) const;
  /// Adjusts the retention bound. Intended to be set once, before publishing;
  /// lowering it below the current size only affects future appends.
  void set_history_limit(std::uint32_t history_limit);

  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] std::size_t history_limit() const;
  [[nodiscard]] std::vector<std::uint64_t> sequences() const;
  [[nodiscard]] Epoch epoch() const;
  [[nodiscard]] Incarnation incarnation() const;
  [[nodiscard]] Generation generation() const;

  /// Number of publications refused by fencing, by code.
  [[nodiscard]] std::uint64_t refusals(Status code) const;

  void clear();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace site_fabric

#endif  // SITE_FABRIC_SNAPSHOT_HPP
