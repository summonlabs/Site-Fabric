// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Versioned, integrity-checked, bounded persistence.
//
// The store is an append-only sequence of self-describing records. Each record
// carries its own length and two digests: one over the payload, one over the
// record header. A reader therefore never trusts a length it has not verified,
// never allocates from an unvalidated count, and can always tell a torn tail
// from a corrupt middle.
//
// Recovery is conservative. A store whose tail is torn is truncated to the
// last record that verified, and the recovery report says so. A store whose
// header is unreadable, or whose format version is not this build's, is
// refused outright: there is no guessing at a layout that might be something
// else.

#ifndef SITE_FABRIC_PERSISTENCE_HPP
#define SITE_FABRIC_PERSISTENCE_HPP

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "site_fabric/snapshot.hpp"

namespace site_fabric {

/// Record tags on the wire and on disk.
enum class RecordType : std::uint16_t {
  UNKNOWN = 0,
  STORE_HEADER = 1,
  SNAPSHOT = 2,
  PUBLICATION = 3,
  RETIREMENT = 4,
  TOMBSTONE = 5,
  LEASE = 6,
  CHECKPOINT = 7,
};

[[nodiscard]] const char* to_string(RecordType type);
[[nodiscard]] bool is_known(RecordType type) noexcept;

/// Store configuration. Every bound is enforced on every append.
struct StoreConfig {
  std::filesystem::path path;
  std::uint64_t max_bytes = limits::kDefaultMaxStoreBytes;
  std::uint32_t max_records = limits::kDefaultMaxStoreRecords;
  std::uint32_t snapshot_history = limits::kDefaultSnapshotHistory;
  /// Flush and fsync after every append. Off by default because the tests
  /// exercise hard kills, and durability of the last record on a kill is not
  /// claimed unless this is on.
  bool durable_commit = false;
};

/// What recovery found.
struct StoreRecoveryReport {
  Status status = Status::UNKNOWN;
  std::uint64_t records_scanned = 0;
  std::uint64_t records_accepted = 0;
  std::uint64_t records_rejected = 0;
  std::uint64_t bytes_consumed = 0;
  std::uint64_t bytes_truncated = 0;
  bool header_present = false;
  bool tail_truncated = false;
  bool version_incompatible = false;
  bool digest_mismatch = false;
  std::vector<std::string> diagnostics;

  [[nodiscard]] bool usable() const noexcept;
  [[nodiscard]] std::string to_string() const;
};

/// Everything a store can yield.
struct StoreContents {
  bool has_snapshot = false;
  SiteSnapshot latest_snapshot;
  std::vector<SiteSnapshot> snapshot_history;
  std::vector<MemberPublicationRecord> publications;
  std::vector<MemberRetirementRecord> retirements;
  std::vector<std::string> tombstones;
  std::vector<AuthorityToken> leases;
  StoreRecoveryReport recovery;

  /// Digest over every accepted payload in order. Two stores that saw the same
  /// records in the same order agree.
  Digest content_digest() const;
  [[nodiscard]] std::size_t record_count() const;
};

/// The append-only store.
///
/// The class holds an open file handle. open() and close() are explicit so the
/// tests can exercise a real close/reopen and a real hard kill at a boundary
/// the test chooses.
class SiteStore {
 public:
  SiteStore();
  ~SiteStore();

  SiteStore(const SiteStore&) = delete;
  SiteStore& operator=(const SiteStore&) = delete;

  /// Opens, or creates, the store. When the file exists it is recovered; a
  /// recovery that found a torn tail rewrites the file to the last good
  /// record before returning.
  Status open(const StoreConfig& config);

  /// Flushes and closes. Safe to call twice.
  Status close();

  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] std::uint64_t bytes() const noexcept;
  [[nodiscard]] std::uint32_t record_count() const noexcept;
  [[nodiscard]] const StoreRecoveryReport& recovery() const noexcept;
  [[nodiscard]] const std::filesystem::path& path() const noexcept;

  Status append_snapshot(const SiteSnapshot& snapshot);
  Status append_publication(const MemberPublicationRecord& record);
  Status append_retirement(const MemberRetirementRecord& record);
  Status append_tombstone(const std::string& tombstone);
  Status append_lease(const AuthorityToken& token);

  /// Loads everything the store holds. Never throws for malformed content; it
  /// reports.
  Status load(StoreContents& out) const;

  /// Rewrites the store keeping only the newest snapshot_history snapshots and
  /// the newest publication per domain. Returns the number of records dropped
  /// through dropped.
  Status compact(std::uint64_t& dropped);

  /// Reads a store from a path without opening it for append. Used by the
  /// inspection tool and by recovery tests.
  [[nodiscard]] static Status read_file(const std::filesystem::path& path,
                                        StoreContents& out);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace site_fabric

#endif  // SITE_FABRIC_PERSISTENCE_HPP
