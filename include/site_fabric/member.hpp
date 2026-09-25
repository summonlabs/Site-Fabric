// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Member-domain declarations.
//
// A declaration is a member domain's complete statement about its own subtree.
// It is sealed: the canonical encoding is hashed, the hash becomes the
// declaration digest, and every later reference to the domain quotes that
// digest. Two runs of the same publisher over the same model therefore produce
// the same digest on any platform, and a byte change anywhere changes it.

#ifndef SITE_FABRIC_MEMBER_HPP
#define SITE_FABRIC_MEMBER_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "site_fabric/evidence.hpp"
#include "site_fabric/failure_domain.hpp"
#include "site_fabric/maintenance.hpp"
#include "site_fabric/resource.hpp"
#include "site_fabric/version.hpp"

namespace site_fabric {

/// A member domain's complete, self-contained statement about its subtree.
struct MemberDomainDeclaration {
  MemberDomainKey domain;
  SiteId site;
  Generation generation;
  SchemaVersion schema{kModelSchemaMajor, kModelSchemaMinor};
  /// Filled by seal(); a declaration is not authoritative before it is sealed.
  Digest digest;
  /// The site epoch the publisher believes it is contributing to. A mismatch
  /// against the composing controller's epoch is reported, never repaired.
  Epoch observed_epoch;

  std::vector<ClusterClaim> clusters;
  std::vector<PodClaim> pods;
  std::vector<RackClaim> racks;
  std::vector<SharedLinkClaim> shared_links;
  std::vector<GatewayClaim> gateways;
  std::vector<CapacityContribution> capacity;
  std::vector<FailureDomainClaim> failure_domains;
  std::vector<MaintenanceZone> maintenance_zones;
  std::vector<ProtectedObligation> obligations;

  Evidence evidence;

  /// Sorts every list into canonical order and removes exact duplicates.
  /// Returns Status::LIMIT_EXCEEDED when a list exceeds its bound and
  /// Status::MALFORMED when a nested record is structurally invalid.
  Status canonicalize();

  /// Digest of the canonical encoding of every content field. The digest field
  /// itself and the evidence source digest are excluded, so sealing is
  /// idempotent.
  [[nodiscard]] Digest compute_digest() const;

  /// canonicalize() followed by compute_digest().
  Status seal();

  /// Structural validation without sealing. Reports the first defect with
  /// factors; it never reports OK for a declaration that would be refused
  /// later.
  Status validate() const;

  [[nodiscard]] bool sealed() const noexcept { return !digest.is_zero(); }
  [[nodiscard]] std::size_t resource_claim_count() const noexcept;
  [[nodiscard]] std::string to_string() const;
};

/// The lifecycle of one member domain as the site sees it.
enum class MemberLifecycle {
  UNKNOWN = 0,
  /// Expected by the site and never heard from.
  ABSENT,
  /// Reported, but the declaration has not been accepted as the current one.
  REPORTED,
  /// Accepted, fresh, and matching the expectation exactly.
  CURRENT,
  /// Accepted once, but its evidence has expired.
  STALE,
  /// A newer generation has been accepted; this one is history.
  SUPERSEDED,
  /// The declaration disagrees with another domain or with the expectation.
  CONFLICTING,
  /// Schema, site or epoch mismatch.
  INCOMPATIBLE,
  /// Retired by an accepted retirement request.
  RETIRED,
  /// Authority revoked by the controller.
  REVOKED,
};

[[nodiscard]] const char* to_string(MemberLifecycle lifecycle);

/// The site's view of one member domain.
struct MemberDomainState {
  MemberDomainKey domain;
  MemberLifecycle lifecycle = MemberLifecycle::UNKNOWN;
  Status status = Status::UNKNOWN;
  Generation generation;
  Digest digest;
  SchemaVersion schema;
  Incarnation incarnation;
  MemberId publisher;
  std::int64_t received_at_ms = 0;
  EvidenceState evidence_state = EvidenceState::UNKNOWN;
  /// True when this domain appears in the site expectation.
  bool expected = false;
  /// True when the expectation names an exact generation and digest.
  bool expectation_pinned = false;
  /// Exact source edge for this member, when one was accepted.
  SourceRef source;
  Factors factors;

  [[nodiscard]] bool authoritative() const noexcept {
    return lifecycle == MemberLifecycle::CURRENT;
  }
  [[nodiscard]] std::string to_string() const;
};

/// One member domain the site expects to hear from.
struct ExpectedMember {
  MemberDomainKey domain;
  /// When pinned, the exact generation the site requires.
  bool pinned = false;
  Generation generation;
  Digest digest;
  /// A required member that never reports makes the site INCOMPLETE. An
  /// optional member that never reports is recorded but does not.
  bool required = true;

  friend bool operator==(const ExpectedMember&, const ExpectedMember&) = default;
};

/// The contract a site controller holds about who must contribute.
struct SiteExpectation {
  SiteId site;
  SchemaVersion schema{kModelSchemaMajor, kModelSchemaMinor};
  Epoch minimum_epoch;
  std::vector<ExpectedMember> members;
  /// When true, a member domain that reports without being expected is a
  /// finding. When false it is recorded as an unexpected contributor.
  bool reject_unexpected = false;

  /// Sorts members into canonical order and refuses duplicates.
  Status canonicalize();
  [[nodiscard]] const ExpectedMember* find(const MemberDomainKey& domain) const noexcept;
  [[nodiscard]] std::size_t required_count() const noexcept;
  [[nodiscard]] std::size_t pinned_count() const noexcept;
};

/// A publication as the controller recorded it.
///
/// The controller never stores a declaration without the authority under which
/// it arrived, which is what lets a later recomposition say exactly which
/// process incarnation supplied each field.
struct MemberPublicationRecord {
  MemberDomainKey domain;
  Generation generation;
  Digest digest;
  SchemaVersion schema;
  SiteId site;
  Epoch observed_epoch;
  Incarnation incarnation;
  MemberId publisher;
  PublishAttempt attempt;
  std::int64_t received_at_ms = 0;
  /// Controller-assigned acceptance order; strictly increasing.
  std::uint64_t acceptance_sequence = 0;
  /// Only present when retain_declaration is on for the controller.
  bool has_declaration = false;
  MemberDomainDeclaration declaration;

  [[nodiscard]] SourceRef source() const { return SourceRef(domain, generation, digest); }
  [[nodiscard]] std::string to_string() const;
};

/// A retirement, as the controller recorded it.
struct MemberRetirementRecord {
  MemberDomainKey domain;
  Generation generation;
  Incarnation incarnation;
  std::int64_t received_at_ms = 0;
  /// Controller-assigned acceptance order; strictly increasing.
  std::uint64_t acceptance_sequence = 0;
  std::string reason;

  [[nodiscard]] std::string to_string() const;
};

}  // namespace site_fabric

#endif  // SITE_FABRIC_MEMBER_HPP
