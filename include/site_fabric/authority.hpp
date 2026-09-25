// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Authority, leases and fencing.
//
// Authority in Site Fabric is never implicit. A process may change site state
// only while it holds a lease whose site, epoch, incarnation and generation
// all match what the registry currently believes. Four independent fences are
// applied to every mutation, and each one has its own status code so a refusal
// can be explained rather than merely denied:
//
//   epoch        - a higher epoch has been observed; the old one never returns
//   incarnation  - this logical holder is now a different process lifetime
//   generation   - this holder already advanced past the offered generation
//   attempt      - this exact publication was already delivered
//
// On top of the fences sits a tombstone table. When a lease holder is observed
// dead, its incarnation is recorded as dead permanently, so a process that
// restarts and replays its previous incarnation is refused even though every
// other field it presents is plausible.

#ifndef SITE_FABRIC_AUTHORITY_HPP
#define SITE_FABRIC_AUTHORITY_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "site_fabric/generation.hpp"
#include "site_fabric/member.hpp"

namespace site_fabric {

/// What a lease authorises.
enum class AuthorityKind {
  UNKNOWN = 0,
  /// The single writer of site snapshots.
  SITE_CONTROLLER,
  /// A member domain's right to publish its own declarations.
  MEMBER_PUBLISHER,
  /// The right to open and close maintenance zones.
  MAINTENANCE_OPERATOR,
  /// Read-only and publish-nothing.
  OBSERVER,
};

[[nodiscard]] const char* to_string(AuthorityKind kind);
[[nodiscard]] bool parse_authority_kind(std::string_view token, AuthorityKind& out);
[[nodiscard]] bool is_valid(AuthorityKind kind) noexcept;
[[nodiscard]] bool may_mutate_site(AuthorityKind kind) noexcept;

/// The lifecycle of a lease.
enum class LeaseState {
  UNKNOWN = 0,
  ACTIVE,
  EXPIRED,
  REVOKED,
  SUPERSEDED,
};

[[nodiscard]] const char* to_string(LeaseState state);

/// A bearer token for one lease.
///
/// The token is a value: copying it does not extend it, and presenting it to a
/// different site, epoch or incarnation is refused by comparison rather than by
/// trust.
struct AuthorityToken {
  AuthorityKind kind = AuthorityKind::UNKNOWN;
  std::string holder;
  SiteId site;
  Epoch epoch;
  Incarnation incarnation;
  Generation generation;
  std::uint64_t lease_id = 0;
  std::int64_t issued_at_ms = 0;
  std::int64_t expires_at_ms = 0;

  [[nodiscard]] bool structurally_valid() const noexcept;

  /// Evaluates the token at an explicit instant. The instant is a parameter,
  /// not a clock read, so a test can place the evaluation anywhere on the
  /// timeline.
  [[nodiscard]] Status evaluate_at(std::int64_t now_ms) const;

  [[nodiscard]] std::string to_string() const;
};

/// Recorded lease state.
struct LeaseRecord {
  AuthorityToken token;
  LeaseState state = LeaseState::UNKNOWN;
  std::int64_t updated_at_ms = 0;
  std::uint64_t renewals = 0;
  std::string note;
};

/// The authority registry.
///
/// Thread-safe by construction: every mutating entry point takes the internal
/// mutex, no entry point calls back into user code while holding it, and no
/// entry point takes a second lock. There is no read-then-write upgrade path,
/// because the few operations that need one are expressed as a single
/// compare-and-set inside one lock acquisition.
class AuthorityRegistry {
 public:
  AuthorityRegistry();
  ~AuthorityRegistry();

  AuthorityRegistry(const AuthorityRegistry&) = delete;
  AuthorityRegistry& operator=(const AuthorityRegistry&) = delete;

  // --- Site controller election -------------------------------------------

  /// Attempts to become, or remain, the site controller.
  ///
  /// Succeeds when the caller holds the current lease and it is not revoked, or
  /// when the lease has expired, or when the caller presents a strictly higher
  /// epoch. Presents AUTHORITY_HELD_ELSEWHERE, FENCED_EPOCH or
  /// FENCED_INCARNATION otherwise.
  Status acquire_site_authority(const std::string& holder, const SiteId& site, Epoch epoch,
                                const Incarnation& incarnation, Generation generation,
                                std::int64_t now_ms, std::int64_t ttl_ms,
                                AuthorityToken& out);

  /// Extends a lease the caller already holds.
  Status renew(const AuthorityToken& token, std::int64_t now_ms, std::int64_t ttl_ms,
               AuthorityToken& out);

  /// Revokes a lease. Revocation is durable and is not undone by expiry.
  Status revoke(const AuthorityToken& token, std::int64_t now_ms, std::string note);

  /// Evaluates a token against the registry and the clock.
  [[nodiscard]] Status validate(const AuthorityToken& token, std::int64_t now_ms) const;

  /// Current epoch the registry has observed for a site.
  [[nodiscard]] Epoch current_epoch(const SiteId& site) const;

  /// The current controller lease, if any.
  [[nodiscard]] bool current_controller(const SiteId& site, AuthorityToken& out) const;

  // --- Member incarnation fencing -----------------------------------------

  /// Records a member domain's process incarnation.
  ///
  /// The first incarnation for a domain is accepted. A higher sequence is
  /// accepted and the previous incarnation is marked dead. The same sequence
  /// with the same boot nonce is REFUSED once that incarnation has been marked
  /// dead, which is what stops a restarted process from resurrecting the
  /// authority it held before the restart. An equal sequence with a different
  /// boot nonce is REFUSED as FENCED_INCARNATION: sequences are issued, never
  /// guessed.
  ///
  /// A sequence of zero means "not yet issued": the registry assigns the next
  /// sequence and reports it through out_sequence, and the caller must use the
  /// assigned value from then on.
  Status admit_member_incarnation(const MemberDomainKey& domain, const Incarnation& incarnation,
                                  std::int64_t now_ms, std::uint64_t* out_sequence);

  /// Marks the recorded incarnation of a domain dead. Idempotent.
  Status retire_member_incarnation(const MemberDomainKey& domain,
                                   const Incarnation& incarnation, std::int64_t now_ms,
                                   std::string reason);

  /// True when the incarnation matches the live one for the domain.
  [[nodiscard]] bool member_incarnation_live(const MemberDomainKey& domain,
                                             const Incarnation& incarnation) const;

  /// The next sequence a domain would be issued.
  [[nodiscard]] std::uint64_t next_member_sequence(const MemberDomainKey& domain) const;

  // --- Publication fencing -------------------------------------------------

  /// Applies the epoch, incarnation, generation and attempt fences to one
  /// publication. Does not record anything.
  [[nodiscard]] Status fence_publication(const MemberDomainKey& domain, Epoch epoch,
                                         const Incarnation& incarnation,
                                         Generation generation, const Digest& digest,
                                         const PublishAttempt& attempt,
                                         std::int64_t now_ms) const;

  /// Records an accepted publication, advancing the per-domain high-water
  /// marks used by fence_publication.
  Status record_publication(const MemberDomainKey& domain, Epoch epoch, Generation generation,
                            const Digest& digest, const PublishAttempt& attempt,
                            std::int64_t now_ms);

  /// Forgets a domain entirely. Only used by tests and by explicit operator
  /// reset; the tombstone table is unaffected.
  void clear_domain(const MemberDomainKey& domain);

  // --- Introspection -------------------------------------------------------

  [[nodiscard]] std::vector<LeaseRecord> leases() const;
  [[nodiscard]] std::vector<std::string> tombstones() const;
  [[nodiscard]] std::size_t domain_count() const;
  [[nodiscard]] std::string to_string() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace site_fabric

#endif  // SITE_FABRIC_AUTHORITY_HPP
