// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The composition engine.
//
// Composition is a pure function of its inputs. Given the same site, the same
// epoch, the same member publications and the same evaluation instant, it
// produces a byte-identical ComposedSite on any platform and in any arrival
// order. Nothing in this header reads a clock, opens a socket, or consults a
// filesystem.
//
// Every authoritative field of the result resolves to a SourceSet: the exact
// member domain generations and digests the field was derived from. A field
// that no source supports is UNKNOWN, never a default.

#ifndef SITE_FABRIC_COMPOSITION_HPP
#define SITE_FABRIC_COMPOSITION_HPP

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "site_fabric/maintenance.hpp"
#include "site_fabric/member.hpp"

namespace site_fabric {

/// The composed state of the site as a whole.
enum class SiteLifecycle {
  UNKNOWN = 0,
  /// No composition has ever run.
  INITIALIZING,
  /// Every expected domain is current, nothing conflicts, every obligation
  /// holds.
  CURRENT,
  /// Every expected domain is current, but something is reduced: a link down,
  /// a gateway degraded, capacity below a soft target.
  DEGRADED,
  /// The site cannot reach part of itself. Reported, never repaired here.
  PARTITIONED,
  /// At least one required domain is absent, stale or incomplete.
  INCOMPLETE,
  /// Two sources disagree about one resource.
  CONFLICTING,
  /// Evidence is missing in a way that prevents a verdict.
  INDETERMINATE,
  /// The controller stopped; the last composed state is historical.
  STOPPED,
};

[[nodiscard]] const char* to_string(SiteLifecycle lifecycle);

/// The site-level decisions a composition produces.
///
/// Each decision is an independently invalidatable unit: a change to a source
/// invalidates exactly the decisions whose SourceSet contains it.
enum class DecisionKind {
  UNKNOWN = 0,
  MEMBERSHIP_COMPLETENESS,
  SITE_EPOCH,
  OWNERSHIP_MAP,
  RACK_TOPOLOGY,
  FAILURE_DOMAIN_FOREST,
  CONNECTIVITY_SUMMARY,
  SHARED_RESOURCE_ACCOUNTING,
  CAPACITY_TOTAL,
  CAPACITY_AVAILABLE,
  MAINTENANCE_EXCLUSION,
  OBLIGATION_VERDICT,
  SITE_LIFECYCLE,
};

[[nodiscard]] const char* to_string(DecisionKind kind);
[[nodiscard]] bool parse_decision_kind(std::string_view token, DecisionKind& out);

/// Stable identity of a decision: its kind plus the resource it is about. A
/// site-wide decision has an empty subject.
struct DecisionId {
  DecisionKind kind = DecisionKind::UNKNOWN;
  ResourceKey subject;

  [[nodiscard]] std::string to_string() const;

  friend bool operator==(const DecisionId&, const DecisionId&) = default;
  friend auto operator<=>(const DecisionId&, const DecisionId&) = default;
};

/// One site-level decision and the exact sources it rests on.
struct SiteDecision {
  DecisionId id;
  Status status = Status::UNKNOWN;
  SourceSet sources;
  Digest value_digest;
  Factors factors;

  [[nodiscard]] bool depends_on(const SourceRef& ref) const {
    return sources.contains(ref);
  }
  [[nodiscard]] bool depends_on_domain(const MemberDomainKey& domain) const {
    return sources.contains_domain(domain);
  }
  [[nodiscard]] std::string to_string() const;
};

/// The composed ownership of one resource.
///
/// exactly one distinct content across all attestors means the resource is
/// attributed; more than one means CONFLICTING and the resource contributes
/// nothing to any aggregate.
struct ResourceAttribution {
  ResourceKey key;
  SourceSet attestors;
  Digest consensus_digest;
  std::size_t distinct_contents = 0;
  Status status = Status::UNKNOWN;
  Factors factors;

  [[nodiscard]] bool attributed() const noexcept {
    return status == Status::OK && distinct_contents == 1;
  }
  [[nodiscard]] std::string to_string() const;
};

/// One line of the capacity ledger.
struct CapacityLedgerEntry {
  ResourceKey owner;
  CapacityScope scope = CapacityScope::UNKNOWN;
  CapacityVector capacity;
  SourceSet attestors;
  /// OK when the entry is counted, CONFLICTING when attestors disagree,
  /// CAPACITY_UNKNOWN when no attestor reported the channel.
  Status status = Status::UNKNOWN;
  /// True when this entry feeds the site total for its scope.
  bool counted_in_total = false;
  Factors factors;

  [[nodiscard]] std::string to_string() const;
};

/// The capacity ledger and its closing identity.
///
/// The identity the ledger must satisfy is
///
///     total == sum of every counted entry
///     available + excluded == total, channel by channel
///
/// and each of those sums is computed with checked arithmetic. When a channel
/// is unknown anywhere in the counted set, the corresponding total channel is
/// unknown and the ledger reports CAPACITY_UNKNOWN for it rather than a
/// smaller number that looks reassuring.
struct CapacityLedger {
  std::vector<CapacityLedgerEntry> entries;
  CapacityVector total;
  CapacityVector available;
  CapacityVector excluded_by_maintenance;
  /// Capacity that could not be attributed and was therefore left out of the
  /// total. Present so the shortfall is visible rather than silent.
  CapacityVector indeterminate;
  bool closes_exactly = false;
  Status status = Status::UNKNOWN;
  Factors factors;

  [[nodiscard]] const CapacityLedgerEntry* find(const ResourceKey& owner) const noexcept;
  [[nodiscard]] std::string to_string() const;
};

/// Composed connectivity counts.
struct ConnectivitySummary {
  std::uint64_t shared_links = 0;
  std::uint64_t shared_links_up = 0;
  std::uint64_t shared_links_down = 0;
  std::uint64_t shared_links_degraded = 0;
  std::uint64_t shared_links_unknown = 0;
  std::uint64_t external_links = 0;
  std::uint64_t internal_links = 0;

  std::uint64_t gateways = 0;
  std::uint64_t gateways_up = 0;
  std::uint64_t gateways_down = 0;
  std::uint64_t gateways_degraded = 0;
  std::uint64_t gateways_unknown = 0;

  /// Resources whose state could not be established, canonicalised.
  std::vector<ResourceKey> unknown_state;
  SourceSet sources;
  Status status = Status::UNKNOWN;

  [[nodiscard]] std::string to_string() const;
};

/// A disagreement the composer refused to resolve on its own.
struct SiteConflict {
  Status code = Status::UNKNOWN;
  ResourceKey subject;
  SourceSet parties;
  std::vector<std::string> variants;
  std::int64_t detected_at_ms = 0;

  [[nodiscard]] std::string to_string() const;
};

/// Something the site expected that did not arrive.
struct SiteDeficit {
  Status code = Status::UNKNOWN;
  MemberDomainKey domain;
  std::string detail;

  [[nodiscard]] std::string to_string() const;
};

/// The composed site.
struct ComposedSite {
  SiteId site;
  Epoch epoch;
  Generation generation;
  Incarnation incarnation;
  std::int64_t composed_at_ms = 0;
  SiteLifecycle lifecycle = SiteLifecycle::UNKNOWN;
  Status status = Status::UNKNOWN;

  bool membership_complete = false;
  bool all_evidence_fresh = false;
  bool any_conflict = false;
  bool any_unknown_capacity = false;

  /// Canonical order: by member domain key.
  std::vector<MemberDomainState> members;
  std::vector<ResourceAttribution> ownership;
  CapacityLedger capacity;
  ConnectivitySummary connectivity;
  FailureDomainForest failure_domains;
  std::vector<MaintenanceEffect> maintenance;
  std::vector<ObligationVerdict> obligations;
  std::vector<SiteConflict> conflicts;
  std::vector<SiteDeficit> deficits;
  /// Canonical order: by decision id.
  std::vector<SiteDecision> decisions;
  Factors factors;

  /// Digest of the canonical encoding of everything above except the digest
  /// itself. Two compositions of the same inputs agree byte for byte.
  [[nodiscard]] Digest compute_digest() const;

  [[nodiscard]] const MemberDomainState* find_member(const MemberDomainKey& domain) const;
  [[nodiscard]] const ResourceAttribution* find_ownership(const ResourceKey& key) const;
  [[nodiscard]] const SiteDecision* find_decision(const DecisionId& id) const;
  [[nodiscard]] std::size_t count_decisions(DecisionKind kind) const;

  [[nodiscard]] std::string to_string() const;
};

/// Everything composition is allowed to look at.
struct CompositionInput {
  SiteId site;
  Epoch epoch;
  Generation generation;
  Incarnation incarnation;
  std::int64_t now_ms = 0;
  SiteExpectation expectation;
  /// Canonical order is imposed by the composer; arrival order is irrelevant.
  std::vector<MemberPublicationRecord> publications;
  std::vector<MemberRetirementRecord> retirements;
  /// Domains whose authority was revoked, canonicalised.
  std::vector<MemberDomainKey> revoked;
  /// Domains whose only evidence is what was read back from disk. Their
  /// declarations are intact, but they are treated as STALE until they report
  /// again: recovered dynamic evidence is history, not a fresh measurement.
  std::vector<MemberDomainKey> reconstructed;
};

/// The pure composition function.
class SiteComposer {
 public:
  SiteComposer() = default;

  /// Composes the site. Returns Status::OK or COMPOSED when a site state was
  /// produced; the ComposedSite is populated even when the status is
  /// CONFLICTING or INCOMPLETE, because the whole point is to report the
  /// defect rather than to withhold the model.
  ///
  /// Status::INVALID and Status::LIMIT_EXCEEDED mean nothing usable was
  /// produced and the out parameter is untouched.
  [[nodiscard]] Status compose(const CompositionInput& input, ComposedSite& out) const;

  /// Bounded work counter of the last composition, for diagnostics.
  [[nodiscard]] std::size_t last_work_units() const noexcept { return last_work_units_; }

 private:
  mutable std::size_t last_work_units_ = 0;
};

/// What a source change invalidates.
struct InvalidationReport {
  MemberDomainKey domain;
  SourceRef changed;
  /// Decisions whose SourceSet contains the changed edge.
  std::vector<DecisionId> invalidated;
  /// Decisions that do not depend on the changed edge.
  std::vector<DecisionId> retained;
  /// The decision kinds that must be recomputed.
  std::vector<DecisionKind> recomposition_required;
  /// Member domains whose state depended on the changed source.
  std::vector<MemberDomainKey> affected_domains;
  /// Resources whose composition depended on the changed source.
  std::vector<ResourceKey> affected_resources;

  [[nodiscard]] bool invalidates_everything() const noexcept;
  [[nodiscard]] std::string to_string() const;
};

/// Computes exactly which decisions a change to one source edge invalidates.
///
/// A decision is invalidated when its SourceSet contains the exact SourceRef
/// that changed. A change to a different generation of the same domain does
/// not invalidate a decision that was derived from this generation only, which
/// is what makes partial invalidation sound.
[[nodiscard]] InvalidationReport invalidate_for_source(const ComposedSite& site,
                                                       const SourceRef& changed);

/// Convenience: invalidates every decision that mentions the domain at all.
[[nodiscard]] InvalidationReport invalidate_for_domain(const ComposedSite& site,
                                                       const MemberDomainKey& domain);

}  // namespace site_fabric

#endif  // SITE_FABRIC_COMPOSITION_HPP
