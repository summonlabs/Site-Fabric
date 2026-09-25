// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The composition engine.
//
// The order of the stages below is the contract, not an implementation detail:
//
//   1. header and expectation validation     - refuse nonsense before reading
//   2. per-domain admission                  - exactly one record per domain
//   3. resource attribution                  - who claims what, and do they agree
//   4. capacity ledger                       - count each attributed unit once
//   5. failure-domain forest                 - nest, detect cycles, detect dangling
//   6. maintenance effects                   - remove from available, never total
//   7. obligation verdicts                   - earned only from available capacity
//   8. membership and deficits               - expected but absent is a finding
//   9. lifecycle                             - one verdict, by fixed precedence
//  10. decisions and provenance              - every field resolves to sources
//
// Nothing here reads a clock, a socket or a file. The evaluation instant is an
// input, so two runs over the same input produce byte-identical output.

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/digest.hpp"
#include "site_fabric/site_fabric.hpp"

namespace site_fabric {
namespace {

// ---------------------------------------------------------------------------
// Channel routing
// ---------------------------------------------------------------------------

/// Which site aggregates a contribution feeds.
///
/// A rack-local pool feeds internal capacity and says nothing about ingress,
/// which therefore stays unreported rather than becoming zero. Getting this
/// wrong in the generous direction is how an aggregate invents capacity.
struct ChannelMask {
  bool ingress = false;
  bool egress = false;
  bool internal = false;
};

[[nodiscard]] ChannelMask mask_for(CapacityScope scope, bool external) {
  switch (scope) {
    case CapacityScope::SITE_INGRESS:
      return ChannelMask{true, false, false};
    case CapacityScope::SITE_EGRESS:
      return ChannelMask{false, true, false};
    case CapacityScope::SITE_INTERNAL:
    case CapacityScope::RACK_LOCAL:
    case CapacityScope::POD_LOCAL:
    case CapacityScope::CLUSTER_LOCAL:
      return ChannelMask{false, false, true};
    case CapacityScope::SHARED_LINK:
      return external ? ChannelMask{true, true, false} : ChannelMask{false, false, true};
    case CapacityScope::GATEWAY:
      return ChannelMask{true, true, false};
    default:
      return ChannelMask{};
  }
}

[[nodiscard]] CapacityScope scope_for(ResourceKind kind) {
  switch (kind) {
    case ResourceKind::RACK:
      return CapacityScope::RACK_LOCAL;
    case ResourceKind::POD:
      return CapacityScope::POD_LOCAL;
    case ResourceKind::CLUSTER:
      return CapacityScope::CLUSTER_LOCAL;
    case ResourceKind::SHARED_LINK:
      return CapacityScope::SHARED_LINK;
    case ResourceKind::GATEWAY:
      return CapacityScope::GATEWAY;
    default:
      return CapacityScope::UNKNOWN;
  }
}

// ---------------------------------------------------------------------------
// Working records
// ---------------------------------------------------------------------------

struct Attestation {
  SourceRef source;
  Digest content;
  bool authoritative = false;
  std::string variant;
};

struct ResourceRecord {
  ResourceKey key;
  std::vector<Attestation> attestations;
  std::size_t distinct_contents = 0;
  Digest consensus;
  Status status = Status::UNKNOWN;
  Factors factors;
  bool all_authoritative = true;

  const RackClaim* rack = nullptr;
  const PodClaim* pod = nullptr;
  const ClusterClaim* cluster = nullptr;
  const SharedLinkClaim* link = nullptr;
  const GatewayClaim* gateway = nullptr;
  const CapacityContribution* pool = nullptr;
  const FailureDomainClaim* domain = nullptr;
  const MaintenanceZone* zone = nullptr;
  const ProtectedObligation* obligation = nullptr;

  [[nodiscard]] SourceSet sources() const {
    SourceSet set;
    for (const auto& attestation : attestations) {
      set.insert(attestation.source);
    }
    return set;
  }
  [[nodiscard]] bool attributed() const {
    return status == Status::OK && distinct_contents == 1;
  }
};

struct Admission {
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
  bool expected = false;
  bool expectation_pinned = false;
  SourceRef source;
  Factors factors;
  const MemberPublicationRecord* record = nullptr;

  [[nodiscard]] bool authoritative() const noexcept {
    return lifecycle == MemberLifecycle::CURRENT;
  }
};

[[nodiscard]] Status evidence_to_status(EvidenceState state) {
  switch (state) {
    case EvidenceState::FRESH:
      return Status::OK;
    case EvidenceState::STALE:
      return Status::EVIDENCE_STALE;
    case EvidenceState::FUTURE:
      return Status::EVIDENCE_FUTURE;
    case EvidenceState::SYNTHETIC_ONLY:
      return Status::EVIDENCE_SYNTHETIC_ONLY;
    case EvidenceState::RECONSTRUCTED_NEEDS_REVALIDATION:
      return Status::EVIDENCE_STALE;
    case EvidenceState::INVALID:
      return Status::INVALID;
    default:
      return Status::EVIDENCE_MISSING;
  }
}

struct LedgerBuild {
  ResourceKey owner;
  CapacityScope scope = CapacityScope::UNKNOWN;
  ChannelMask mask;
  bool external = false;
  CapacityVector capacity;
  SourceSet attestors;
  Status status = Status::UNKNOWN;
  bool counted = false;
  Factors factors;
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

void add_conflict(ComposedSite& site, Status code, const ResourceKey& subject,
                  const SourceSet& parties, std::vector<std::string> variants) {
  if (site.conflicts.size() >= limits::kMaxConflicts) {
    site.factors.add("conflicts_truncated", "true");
    return;
  }
  SiteConflict conflict;
  conflict.code = code;
  conflict.subject = subject;
  conflict.parties = parties;
  conflict.variants = std::move(variants);
  conflict.detected_at_ms = site.composed_at_ms;
  site.conflicts.push_back(std::move(conflict));
}

void add_deficit(ComposedSite& site, Status code, const MemberDomainKey& domain,
                 std::string detail) {
  if (site.deficits.size() >= limits::kMaxDeficits) {
    site.factors.add("deficits_truncated", "true");
    return;
  }
  SiteDeficit deficit;
  deficit.code = code;
  deficit.domain = domain;
  deficit.detail = std::move(detail);
  site.deficits.push_back(std::move(deficit));
}

void add_decision(ComposedSite& site, DecisionKind kind, ResourceKey subject, Status status,
                  const SourceSet& sources, Factors factors) {
  if (site.decisions.size() >= limits::kMaxDecisions) {
    site.factors.add("decisions_truncated", "true");
    return;
  }
  SiteDecision decision;
  decision.id.kind = kind;
  decision.id.subject = std::move(subject);
  decision.status = status;
  decision.sources = sources;
  decision.factors = std::move(factors);
  decision.value_digest = internal::decision_value_digest(decision);
  site.decisions.push_back(std::move(decision));
}

void record_attestation(ResourceRecord& record, const Attestation& attestation) {
  for (auto& existing : record.attestations) {
    if (existing.source == attestation.source) {
      return;
    }
  }
  if (record.attestations.size() >= limits::kMaxAttestorsPerResource) {
    record.factors.add("attestors_truncated", "true");
    return;
  }
  record.attestations.push_back(attestation);
}

/// Adds one ledger entry's masked channels into an aggregate.
void add_aggregate(CapacityAggregate& aggregate, const LedgerBuild& build) {
  aggregate.count_entry();
  if (build.mask.ingress) {
    aggregate.add_channel(CapacityChannel::INGRESS, build.capacity.ingress);
  }
  if (build.mask.egress) {
    aggregate.add_channel(CapacityChannel::EGRESS, build.capacity.egress);
  }
  if (build.mask.internal) {
    aggregate.add_channel(CapacityChannel::INTERNAL, build.capacity.internal);
  }
}

/// Finalises the agreement state of a resource from its attestations.
void resolve_resource(ResourceRecord& record) {
  std::vector<Digest> distinct;
  for (const auto& attestation : record.attestations) {
    if (std::find(distinct.begin(), distinct.end(), attestation.content) == distinct.end()) {
      distinct.push_back(attestation.content);
    }
  }
  std::sort(distinct.begin(), distinct.end());
  record.distinct_contents = distinct.size();
  record.all_authoritative = true;
  for (const auto& attestation : record.attestations) {
    if (!attestation.authoritative) {
      record.all_authoritative = false;
    }
  }

  const bool exclusive = record.key.kind == ResourceKind::RACK ||
                         record.key.kind == ResourceKind::POD ||
                         record.key.kind == ResourceKind::CLUSTER ||
                         record.key.kind == ResourceKind::FAILURE_DOMAIN ||
                         record.key.kind == ResourceKind::MAINTENANCE_ZONE ||
                         record.key.kind == ResourceKind::OBLIGATION;

  if (record.attestations.empty()) {
    record.status = Status::NO_AUTHORITATIVE_SOURCE;
    record.factors.add("attestors", "0");
    return;
  }
  if (record.distinct_contents > 1) {
    record.status = exclusive ? Status::OWNERSHIP_OVERLAP : Status::ACCOUNTING_CONFLICT;
    record.factors.add("distinct_contents", std::to_string(record.distinct_contents));
    record.factors.add("attestors", std::to_string(record.attestations.size()));
    for (const auto& attestation : record.attestations) {
      record.factors.add("variant", attestation.content.short_hex());
    }
    return;
  }
  record.consensus = distinct.front();
  if (!record.all_authoritative) {
    record.status = Status::NO_AUTHORITATIVE_SOURCE;
    record.factors.add("non_authoritative_attestor", "true");
    return;
  }
  record.status = Status::OK;
}

// ---------------------------------------------------------------------------
// Composer
// ---------------------------------------------------------------------------

class Composer {
 public:
  Composer(const CompositionInput& input, ComposedSite& out) : input_(input), out_(out) {}

  [[nodiscard]] Status run();

 private:
  [[nodiscard]] Status prepare();
  [[nodiscard]] Status admit_members();
  [[nodiscard]] Status attribute_resources();
  [[nodiscard]] Status build_failure_domains();
  [[nodiscard]] Status build_maintenance();
  [[nodiscard]] Status build_obligations();
  void build_connectivity();
  void build_membership();
  void build_lifecycle();
  void build_decisions();

  void collect_zone_scope(const MaintenanceZone& zone, std::vector<ResourceKey>& keys) const;
  [[nodiscard]] std::vector<LedgerBuild> collect_ledger_builds() const;
  [[nodiscard]] bool obligation_overlaps_zone(const ProtectedObligation& obligation,
                                              const MaintenanceZone& zone) const;

  const CompositionInput& input_;
  ComposedSite& out_;

  SiteExpectation expectation_;
  std::map<MemberDomainKey, Admission> admissions_;
  std::map<ResourceKey, ResourceRecord> resources_;
  std::map<FailureDomainId, std::vector<ResourceKey>> domain_members_;
  std::map<MaintenanceZoneId, std::vector<ResourceKey>> zone_scope_;
  SourceSet all_sources_;
  bool maintenance_indeterminate_ = false;
};

Status Composer::prepare() {
  if (!input_.site.valid()) {
    return Status::INVALID;
  }
  if (!input_.epoch.is_set() || !input_.generation.is_set()) {
    return Status::INVALID;
  }
  if (!input_.incarnation.valid()) {
    return Status::INVALID;
  }
  if (input_.now_ms <= 0) {
    return Status::INVALID;
  }
  if (input_.publications.size() > limits::kMaxMemberDomains) {
    return Status::LIMIT_EXCEEDED;
  }
  if (input_.retirements.size() > limits::kMaxMemberDomains) {
    return Status::LIMIT_EXCEEDED;
  }
  if (input_.revoked.size() > limits::kMaxMemberDomains) {
    return Status::LIMIT_EXCEEDED;
  }

  expectation_ = input_.expectation;
  if (expectation_.site.empty()) {
    expectation_.site = input_.site;
  }
  if (!(expectation_.site == input_.site)) {
    return Status::SITE_MISMATCH;
  }
  const Status canonical = expectation_.canonicalize();
  if (!is_ok(canonical)) {
    return canonical;
  }

  out_.site = input_.site;
  out_.epoch = input_.epoch;
  out_.generation = input_.generation;
  out_.incarnation = input_.incarnation;
  out_.composed_at_ms = input_.now_ms;
  return Status::OK;
}

Status Composer::admit_members() {
  // Exactly one record per domain: the highest generation, then the highest
  // attempt, then the highest acceptance sequence. Ties are resolved by the
  // declaration digest so the choice never depends on iteration order.
  std::map<MemberDomainKey, const MemberPublicationRecord*> best;
  for (const auto& record : input_.publications) {
    if (!record.domain.valid() || record.generation.is_unset() || record.digest.is_zero()) {
      continue;
    }
    const auto found = best.find(record.domain);
    if (found == best.end()) {
      best[record.domain] = &record;
      continue;
    }
    const MemberPublicationRecord* incumbent = found->second;
    const bool replace =
        record.generation > incumbent->generation ||
        (record.generation == incumbent->generation && record.attempt > incumbent->attempt) ||
        (record.generation == incumbent->generation && record.attempt == incumbent->attempt &&
         record.acceptance_sequence > incumbent->acceptance_sequence);
    if (replace) {
      found->second = &record;
    }
  }

  std::set<MemberDomainKey> retired;
  for (const auto& retirement : input_.retirements) {
    retired.insert(retirement.domain);
  }
  std::set<MemberDomainKey> revoked;
  for (const auto& domain : input_.revoked) {
    revoked.insert(domain);
  }

  for (const auto& [domain, record] : best) {
    Admission admission;
    admission.domain = domain;
    admission.generation = record->generation;
    admission.digest = record->digest;
    admission.schema = record->schema;
    admission.incarnation = record->incarnation;
    admission.publisher = record->publisher;
    admission.received_at_ms = record->received_at_ms;
    admission.record = record;
    admission.source = record->source();

    const bool reconstructed =
        std::find(input_.reconstructed.begin(), input_.reconstructed.end(), domain) !=
        input_.reconstructed.end();

    const ExpectedMember* expected = expectation_.find(domain);
    admission.expected = expected != nullptr;
    admission.expectation_pinned = expected != nullptr && expected->pinned;
    admission.evidence_state = record->has_declaration
                                   ? record->declaration.evidence.evaluate(input_.now_ms)
                                   : EvidenceState::UNKNOWN;

    if (revoked.count(domain) != 0) {
      admission.lifecycle = MemberLifecycle::REVOKED;
      admission.status = Status::LEASE_REVOKED;
      admission.factors.add("revoked", "true");
    } else if (retired.count(domain) != 0) {
      admission.lifecycle = MemberLifecycle::RETIRED;
      admission.status = Status::ALREADY_RETIRED;
      admission.factors.add("retired", "true");
    } else if (!(record->site == input_.site)) {
      admission.lifecycle = MemberLifecycle::INCOMPATIBLE;
      admission.status = Status::SITE_MISMATCH;
      admission.factors.add("site", record->site.value());
    } else if (!record->schema.compatible_with(SchemaVersion{kModelSchemaMajor, kModelSchemaMinor})) {
      admission.lifecycle = MemberLifecycle::INCOMPATIBLE;
      admission.status = Status::SCHEMA_INCOMPATIBLE;
      admission.factors.add("schema", record->schema.to_string());
    } else if (record->observed_epoch > input_.epoch) {
      admission.lifecycle = MemberLifecycle::INCOMPATIBLE;
      admission.status = Status::EPOCH_MISMATCH;
      admission.factors.add("observed_epoch", record->observed_epoch.to_string());
      admission.factors.add("site_epoch", input_.epoch.to_string());
    } else if (!record->has_declaration) {
      admission.lifecycle = MemberLifecycle::REPORTED;
      admission.status = Status::PARTIAL;
      admission.factors.add("declaration", "not_retained");
    } else if (record->declaration.digest != record->digest) {
      admission.lifecycle = MemberLifecycle::CONFLICTING;
      admission.status = Status::DIGEST_MISMATCH_DECLARATION;
      admission.factors.add("record_digest", record->digest.short_hex());
      admission.factors.add("declaration_digest", record->declaration.digest.short_hex());
    } else {
      const Digest recomputed = record->declaration.compute_digest();
      if (!(recomputed == record->digest)) {
        admission.lifecycle = MemberLifecycle::CONFLICTING;
        admission.status = Status::DIGEST_MISMATCH_DECLARATION;
        admission.factors.add("recomputed", recomputed.short_hex());
      } else {
        const Status declared = record->declaration.validate();
        if (!is_ok(declared)) {
          admission.lifecycle = MemberLifecycle::INCOMPATIBLE;
          admission.status = declared;
          admission.factors.add("declaration_invalid", site_fabric::to_string(declared));
        } else if (expected != nullptr && expected->pinned) {
          if (!(expected->generation == record->generation)) {
            admission.lifecycle = MemberLifecycle::CONFLICTING;
            admission.status = Status::GENERATION_MISMATCH;
            admission.factors.add("expected_generation", expected->generation.to_string());
            admission.factors.add("actual_generation", record->generation.to_string());
          } else if (!(expected->digest == record->digest)) {
            admission.lifecycle = MemberLifecycle::CONFLICTING;
            admission.status = Status::DIGEST_MISMATCH_DECLARATION;
            admission.factors.add("expected_digest", expected->digest.short_hex());
            admission.factors.add("actual_digest", record->digest.short_hex());
          }
        } else if (expected == nullptr && expectation_.reject_unexpected) {
          admission.lifecycle = MemberLifecycle::INCOMPATIBLE;
          admission.status = Status::MEMBERSHIP_UNEXPECTED;
          admission.factors.add("unexpected_domain", domain.to_string());
        }

        if (admission.lifecycle == MemberLifecycle::UNKNOWN && reconstructed) {
          // Recovered from disk. The declaration is intact, which is why its
          // digest still verifies, but it is a record of the past and is
          // treated as stale until the domain reports again.
          admission.lifecycle = MemberLifecycle::STALE;
          admission.status = Status::EVIDENCE_STALE;
          admission.factors.add("provenance", "RECONSTRUCTED");
          admission.factors.add("source", "recovered");
        }
        if (admission.lifecycle == MemberLifecycle::UNKNOWN) {
          switch (admission.evidence_state) {
            case EvidenceState::FRESH:
              admission.lifecycle = MemberLifecycle::CURRENT;
              admission.status = Status::CURRENT;
              break;
            case EvidenceState::SYNTHETIC_ONLY:
              admission.lifecycle = MemberLifecycle::REPORTED;
              admission.status = Status::EVIDENCE_SYNTHETIC_ONLY;
              admission.factors.add("provenance", "SYNTHETIC");
              break;
            case EvidenceState::RECONSTRUCTED_NEEDS_REVALIDATION:
              admission.lifecycle = MemberLifecycle::STALE;
              admission.status = Status::EVIDENCE_STALE;
              admission.factors.add("provenance", "RECONSTRUCTED");
              break;
            default:
              admission.lifecycle = MemberLifecycle::STALE;
              admission.status = evidence_to_status(admission.evidence_state);
              admission.factors.add("evidence", site_fabric::to_string(admission.evidence_state));
              break;
          }
        }
        if (!admission.expected && expectation_.reject_unexpected == false &&
            admission.lifecycle == MemberLifecycle::CURRENT) {
          admission.factors.add("unexpected_domain", domain.to_string());
        }
      }
    }

    out_.members.push_back(MemberDomainState{});
    MemberDomainState& state = out_.members.back();
    state.domain = domain;
    state.lifecycle = admission.lifecycle;
    state.status = admission.status;
    state.generation = admission.generation;
    state.digest = admission.digest;
    state.schema = admission.schema;
    state.incarnation = admission.incarnation;
    state.publisher = admission.publisher;
    state.received_at_ms = admission.received_at_ms;
    state.evidence_state = admission.evidence_state;
    state.expected = admission.expected;
    state.expectation_pinned = admission.expectation_pinned;
    state.source = admission.source;
    state.factors = admission.factors;

    admissions_[domain] = std::move(admission);
    all_sources_.insert(record->source());
  }
  return Status::OK;
}

Status Composer::attribute_resources() {
  for (const auto& [domain, admission] : admissions_) {
    if (admission.record == nullptr || !admission.record->has_declaration) {
      continue;
    }
    const MemberDomainDeclaration& declaration = admission.record->declaration;
    const bool authoritative = admission.authoritative();
    const SourceRef source = admission.source;

    const auto attest = [&](const ResourceKey& key, const Digest& content,
                            const std::string& variant) {
      ResourceRecord& record = resources_[key];
      record.key = key;
      Attestation attestation;
      attestation.source = source;
      attestation.content = content;
      attestation.authoritative = authoritative;
      attestation.variant = variant;
      record_attestation(record, attestation);
    };

    for (const auto& claim : declaration.clusters) {
      attest(ResourceKey::cluster(claim.id.value()), internal::claim_digest(claim),
             admission.domain.to_string());
      resources_[ResourceKey::cluster(claim.id.value())].cluster = &claim;
    }
    for (const auto& claim : declaration.pods) {
      attest(ResourceKey::pod(claim.id.value()), internal::claim_digest(claim),
             admission.domain.to_string());
      resources_[ResourceKey::pod(claim.id.value())].pod = &claim;
    }
    for (const auto& claim : declaration.racks) {
      attest(ResourceKey::rack(claim.id.value()), internal::claim_digest(claim),
             admission.domain.to_string());
      resources_[ResourceKey::rack(claim.id.value())].rack = &claim;
    }
    for (const auto& claim : declaration.shared_links) {
      attest(ResourceKey::shared_link(claim.id.value()), internal::claim_digest(claim),
             admission.domain.to_string());
      resources_[ResourceKey::shared_link(claim.id.value())].link = &claim;
    }
    for (const auto& claim : declaration.gateways) {
      attest(ResourceKey::gateway(claim.id.value()), internal::claim_digest(claim),
             admission.domain.to_string());
      resources_[ResourceKey::gateway(claim.id.value())].gateway = &claim;
    }
    for (const auto& claim : declaration.capacity) {
      attest(claim.owner, internal::claim_digest(claim), admission.domain.to_string());
      resources_[claim.owner].pool = &claim;
    }
    for (const auto& claim : declaration.failure_domains) {
      attest(ResourceKey::failure_domain(claim.id.value()), internal::claim_digest(claim),
             admission.domain.to_string());
      resources_[ResourceKey::failure_domain(claim.id.value())].domain = &claim;
    }
    for (const auto& claim : declaration.maintenance_zones) {
      attest(ResourceKey::maintenance_zone(claim.id.value()), internal::claim_digest(claim),
             admission.domain.to_string());
      resources_[ResourceKey::maintenance_zone(claim.id.value())].zone = &claim;
    }
    for (const auto& claim : declaration.obligations) {
      attest(ResourceKey::obligation(claim.id.value()), internal::claim_digest(claim),
             admission.domain.to_string());
      resources_[ResourceKey::obligation(claim.id.value())].obligation = &claim;
    }
  }

  for (auto& [key, record] : resources_) {
    resolve_resource(record);
    ResourceAttribution attribution;
    attribution.key = key;
    attribution.attestors = record.sources();
    attribution.consensus_digest = record.consensus;
    attribution.distinct_contents = record.distinct_contents;
    attribution.status = record.status;
    attribution.factors = record.factors;
    out_.ownership.push_back(std::move(attribution));

    if (record.status == Status::OWNERSHIP_OVERLAP) {
      add_conflict(out_, Status::OWNERSHIP_OVERLAP, key, record.sources(), {});
    } else if (record.status == Status::ACCOUNTING_CONFLICT) {
      add_conflict(out_, Status::ACCOUNTING_CONFLICT, key, record.sources(), {});
    }
  }
  return Status::OK;
}

std::vector<LedgerBuild> Composer::collect_ledger_builds() const {
  std::vector<LedgerBuild> builds;
  for (const auto& [key, record] : resources_) {
    if (record.rack == nullptr && record.link == nullptr && record.gateway == nullptr &&
        record.pool == nullptr) {
      continue;
    }
    LedgerBuild build;
    build.owner = key;
    build.attestors = record.sources();
    if (record.rack != nullptr) {
      build.scope = CapacityScope::RACK_LOCAL;
      build.mask = mask_for(build.scope, false);
      build.capacity = record.rack->local_capacity;
    } else if (record.link != nullptr) {
      build.scope = CapacityScope::SHARED_LINK;
      build.external = record.link->external;
      build.mask = mask_for(build.scope, build.external);
      build.capacity = record.link->capacity;
    } else if (record.gateway != nullptr) {
      build.scope = CapacityScope::GATEWAY;
      build.mask = mask_for(build.scope, true);
      build.capacity = record.gateway->capacity;
    } else {
      build.scope = record.pool->scope;
      build.mask = mask_for(build.scope, false);
      build.capacity = record.pool->capacity;
    }
    build.factors = record.factors;

    if (record.status != Status::OK) {
      build.status =
          record.status == Status::UNKNOWN ? Status::NO_AUTHORITATIVE_SOURCE : record.status;
      build.counted = false;
      builds.push_back(std::move(build));
      continue;
    }
    if (build.mask.ingress == false && build.mask.egress == false &&
        build.mask.internal == false) {
      build.status = Status::UNSUPPORTED;
      build.counted = false;
      build.factors.add("scope", "does_not_feed_site_total");
      builds.push_back(std::move(build));
      continue;
    }
    const bool channel_unknown = (build.mask.ingress && !build.capacity.ingress.known) ||
                                 (build.mask.egress && !build.capacity.egress.known) ||
                                 (build.mask.internal && !build.capacity.internal.known);
    build.status = channel_unknown ? Status::CAPACITY_UNKNOWN : Status::OK;
    build.counted = true;
    builds.push_back(std::move(build));
  }
  return builds;
}

Status Composer::build_failure_domains() {
  std::map<FailureDomainId, FailureDomainNode> nodes;
  std::map<FailureDomainId, std::vector<ResourceRecord*>> claim_records;

  for (auto& [key, record] : resources_) {
    if (key.kind != ResourceKind::FAILURE_DOMAIN) {
      continue;
    }
    FailureDomainId id = FailureDomainId::unchecked(key.id);
    claim_records[id].push_back(&record);

    FailureDomainNode& node = nodes[id];
    node.id = id;
    node.attestors = record.sources();
    node.status = record.status;
    if (record.domain != nullptr) {
      node.domain_class = record.domain->domain_class;
      node.parent = record.domain->parent;
    }
  }

  // Union of member lists across attestations, canonicalised.
  for (auto& [id, record] : claim_records) {
    FailureDomainNode& node = nodes[id];
    for (const auto& attestation : record.front()->attestations) {
      const MemberPublicationRecord* publication = nullptr;
      const auto found = admissions_.find(attestation.source.domain);
      if (found != admissions_.end()) {
        publication = found->second.record;
      }
      if (publication == nullptr || !publication->has_declaration) {
        continue;
      }
      for (const auto& claim : publication->declaration.failure_domains) {
        if (claim.id != id) {
          continue;
        }
        node.members.insert(node.members.end(), claim.members.begin(), claim.members.end());
      }
    }
    std::sort(node.members.begin(), node.members.end());
    node.members.erase(std::unique(node.members.begin(), node.members.end()), node.members.end());
    if (node.members.size() > limits::kMaxResourceMembers) {
      node.members.resize(limits::kMaxResourceMembers);
      node.status = Status::LIMIT_EXCEEDED;
    }
    domain_members_[id] = node.members;
  }

  FailureDomainForest forest;
  for (auto& [id, node] : nodes) {
    if (node.status == Status::UNKNOWN) {
      node.status = Status::OK;
    } else if (node.status == Status::OWNERSHIP_OVERLAP) {
      // Two attestors disagreed about the class or the parent.
      node.status = Status::FAILURE_DOMAIN_CONFLICT;
    }
    // NO_AUTHORITATIVE_SOURCE is left as it is: a domain nobody authoritative
    // has described is missing evidence, not a disagreement, and the two must
    // not be reported as the same thing.
    if (node.status == Status::NO_AUTHORITATIVE_SOURCE) {
      forest.factors.add("unsourced_domain", id.value());
    }
    if (node.parent.has_value() && nodes.find(*node.parent) == nodes.end()) {
      node.status = Status::FAILURE_DOMAIN_DANGLING_PARENT;
      forest.dangling.push_back(id);
      add_deficit(out_, Status::FAILURE_DOMAIN_DANGLING_PARENT,
                  MemberDomainKey{}, "parent=" + node.parent->value());
    }
    if (node.domain_class == FailureDomainClass::UNKNOWN) {
      node.status = Status::FAILURE_DOMAIN_CONFLICT;
    }
    forest.nodes.push_back(std::move(node));
  }
  std::sort(forest.nodes.begin(), forest.nodes.end(),
            [](const FailureDomainNode& left, const FailureDomainNode& right) {
              return left.id < right.id;
            });
  forest.resolve_ancestry();

  if (!forest.cyclic.empty()) {
    forest.status = Status::FAILURE_DOMAIN_CYCLE;
    forest.factors.add("cyclic_domains", std::to_string(forest.cyclic.size()));
    std::vector<std::string> cycle_names;
    cycle_names.reserve(forest.cyclic.size());
    for (const auto& domain : forest.cyclic) {
      cycle_names.push_back(domain.value());
    }
    add_conflict(out_, Status::FAILURE_DOMAIN_CYCLE, ResourceKey{}, SourceSet{},
                 std::move(cycle_names));
  } else if (!forest.dangling.empty()) {
    forest.status = Status::FAILURE_DOMAIN_DANGLING_PARENT;
    forest.factors.add("dangling_domains", std::to_string(forest.dangling.size()));
  } else {
    bool any_conflict = false;
    bool any_unsourced = false;
    for (const auto& node : forest.nodes) {
      if (node.status == Status::FAILURE_DOMAIN_CONFLICT) {
        any_conflict = true;
      } else if (node.status == Status::NO_AUTHORITATIVE_SOURCE) {
        any_unsourced = true;
      }
    }
    if (any_conflict) {
      forest.status = Status::FAILURE_DOMAIN_CONFLICT;
    } else if (any_unsourced) {
      forest.status = Status::NO_AUTHORITATIVE_SOURCE;
    } else {
      forest.status = Status::OK;
    }
  }

  for (const auto& node : forest.nodes) {
    if (node.status == Status::FAILURE_DOMAIN_CONFLICT) {
      const auto found = resources_.find(ResourceKey::failure_domain(node.id.value()));
      if (found != resources_.end()) {
        add_conflict(out_, Status::FAILURE_DOMAIN_CONFLICT, found->first, found->second.sources(),
                     {});
      }
    }
  }

  out_.failure_domains = std::move(forest);
  return Status::OK;
}

void Composer::collect_zone_scope(const MaintenanceZone& zone, std::vector<ResourceKey>& keys) const {
  keys.insert(keys.end(), zone.resources.begin(), zone.resources.end());

  for (const auto& domain : zone.domains) {
    const auto direct = domain_members_.find(domain);
    if (direct != domain_members_.end()) {
      keys.insert(keys.end(), direct->second.begin(), direct->second.end());
    }
    for (const auto& node : out_.failure_domains.nodes) {
      if (std::find(node.ancestors.begin(), node.ancestors.end(), domain) != node.ancestors.end()) {
        keys.insert(keys.end(), node.members.begin(), node.members.end());
      }
    }
  }
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
}

bool Composer::obligation_overlaps_zone(const ProtectedObligation& obligation,
                                        const MaintenanceZone& zone) const {
  if (!obligation.protected_from_maintenance) {
    return false;
  }
  if (obligation.scope_resource.has_value()) {
    const auto found = zone_scope_.find(zone.id);
    if (found != zone_scope_.end() &&
        std::find(found->second.begin(), found->second.end(), *obligation.scope_resource) !=
            found->second.end()) {
      return true;
    }
  }
  if (obligation.scope_domain.has_value()) {
    if (std::find(zone.domains.begin(), zone.domains.end(), *obligation.scope_domain) !=
        zone.domains.end()) {
      return true;
    }
    const auto found = zone_scope_.find(zone.id);
    if (found != zone_scope_.end()) {
      for (const auto& key : found->second) {
        if (key.kind == ResourceKind::FAILURE_DOMAIN &&
            key.id == obligation.scope_domain->value()) {
          return true;
        }
      }
    }
    const FailureDomainNode* node = out_.failure_domains.find(*obligation.scope_domain);
    if (node != nullptr) {
      for (const auto& domain : zone.domains) {
        if (std::find(node->ancestors.begin(), node->ancestors.end(), domain) !=
            node->ancestors.end()) {
          return true;
        }
      }
    }
  }
  return false;
}

Status Composer::build_maintenance() {
  // Zone scopes first: every zone needs its resource set before overlaps can be
  // decided.
  for (const auto& [key, record] : resources_) {
    if (key.kind != ResourceKind::MAINTENANCE_ZONE || record.zone == nullptr) {
      continue;
    }
    std::vector<ResourceKey> scope;
    if (record.status == Status::OK) {
      collect_zone_scope(*record.zone, scope);
    }
    zone_scope_[record.zone->id] = std::move(scope);
  }

  for (const auto& [key, record] : resources_) {
    if (key.kind != ResourceKind::MAINTENANCE_ZONE || record.zone == nullptr) {
      continue;
    }
    MaintenanceEffect effect;
    effect.id = record.zone->id;
    effect.state = record.zone->state;
    effect.status = Status::OK;

    if (record.status != Status::OK) {
      effect.status = record.status;
      effect.factors.add("zone_attribution", site_fabric::to_string(record.status));
      out_.maintenance.push_back(std::move(effect));
      continue;
    }

    const auto scope = zone_scope_.find(effect.id);
    const std::vector<ResourceKey> empty;
    const std::vector<ResourceKey>& keys = scope == zone_scope_.end() ? empty : scope->second;

    // A zone over a protected obligation is refused, not applied.
    bool refused = false;
    for (const auto& [obligation_key, obligation_record] : resources_) {
      if (obligation_key.kind != ResourceKind::OBLIGATION ||
          obligation_record.obligation == nullptr ||
          obligation_record.status != Status::OK) {
        continue;
      }
      if (obligation_overlaps_zone(*obligation_record.obligation, *record.zone)) {
        effect.overlapping_obligations.push_back(obligation_record.obligation->id);
        refused = true;
      }
    }
    std::sort(effect.overlapping_obligations.begin(), effect.overlapping_obligations.end());
    effect.overlapping_obligations.erase(
        std::unique(effect.overlapping_obligations.begin(), effect.overlapping_obligations.end()),
        effect.overlapping_obligations.end());

    if (refused) {
      effect.status = Status::MAINTENANCE_CONFLICT;
      effect.factors.add("overlapping_obligations",
                         std::to_string(effect.overlapping_obligations.size()));
      add_conflict(out_, Status::MAINTENANCE_CONFLICT,
                   ResourceKey::maintenance_zone(effect.id.value()), record.sources(), {});
      out_.maintenance.push_back(std::move(effect));
      continue;
    }

    if (record.zone->state == MaintenanceState::UNKNOWN) {
      // The operator has not said whether this zone is active. The capacity is
      // held out of availability rather than quietly counted as usable, and
      // both the zone status and the site lifecycle say why.
      effect.status = Status::MAINTENANCE_UNKNOWN;
      effect.excluded = keys;
      effect.factors.add("state", "UNKNOWN");
      out_.maintenance.push_back(std::move(effect));
      continue;
    }

    if (!record.zone->window_contains(input_.now_ms) &&
        record.zone->state == MaintenanceState::ACTIVE) {
      effect.factors.add("window", "does_not_contain_now");
    }

    if (excludes_capacity(record.zone->state)) {
      effect.excluded = keys;
    }
    out_.maintenance.push_back(std::move(effect));
  }

  std::sort(out_.maintenance.begin(), out_.maintenance.end(),
            [](const MaintenanceEffect& left, const MaintenanceEffect& right) {
              return left.id < right.id;
            });

  // Fold maintenance into the ledger now that the exclusion sets are known.
  //
  // The disposition of a counted entry is one of three:
  //
  //   available      counted, not affected by maintenance
  //   excluded       counted, removed from available by an ACTIVE zone
  //   unknown        counted, in a zone whose state was never reported
  //
  // The third case is also treated as excluded. An operator who has not said
  // whether a zone is active has not said the capacity is for sale, so the
  // conservative reading is that it is not; the zone's own status and the site
  // lifecycle both report the uncertainty. Capacity that could not be
  // attributed at all is not part of the total and is reported as
  // indeterminate instead.
  std::set<ResourceKey> excluded_keys;
  bool unknown_maintenance = false;
  for (const auto& effect : out_.maintenance) {
    if (effect.status == Status::MAINTENANCE_CONFLICT) {
      continue;
    }
    if (effect.status == Status::MAINTENANCE_UNKNOWN) {
      unknown_maintenance = true;
    }
    if (excludes_capacity(effect.state) || effect.status == Status::MAINTENANCE_UNKNOWN) {
      for (const auto& key : effect.excluded) {
        excluded_keys.insert(key);
      }
    }
  }
  maintenance_indeterminate_ = unknown_maintenance;

  CapacityAggregate total_aggregate;
  CapacityAggregate available_aggregate;
  CapacityAggregate excluded_aggregate;
  CapacityAggregate indeterminate_aggregate;

  for (const auto& build : collect_ledger_builds()) {
    CapacityLedgerEntry ledger_entry;
    ledger_entry.owner = build.owner;
    ledger_entry.scope = build.scope;
    ledger_entry.capacity = build.capacity;
    ledger_entry.attestors = build.attestors;
    ledger_entry.status = build.status;
    ledger_entry.factors = build.factors;

    if (!build.counted) {
      ledger_entry.counted_in_total = false;
      add_aggregate(indeterminate_aggregate, build);
      out_.capacity.entries.push_back(std::move(ledger_entry));
      continue;
    }

    // Counted entries are always part of the total: maintenance takes capacity
    // out of what is available, never out of what the site owns.
    add_aggregate(total_aggregate, build);

    if (excluded_keys.count(build.owner) != 0) {
      ledger_entry.counted_in_total = false;
      ledger_entry.factors.add("maintenance", "excluded");
      add_aggregate(excluded_aggregate, build);
      out_.capacity.entries.push_back(std::move(ledger_entry));
      continue;
    }

    ledger_entry.counted_in_total = true;
    add_aggregate(available_aggregate, build);
    out_.capacity.entries.push_back(std::move(ledger_entry));
  }

  out_.capacity.total = total_aggregate.total();
  out_.capacity.indeterminate = indeterminate_aggregate.total();

  // A channel the total knows and nothing excluded is excluded by zero, not by
  // nothing: the exclusion is a fact about the site, not an unreported value.
  CapacityVector excluded_value = excluded_aggregate.total();
  const auto zero_fill = [&](CapacityChannel channel, CapacityValue& value) {
    if (!excluded_aggregate.channel_seen(channel) && total_aggregate.channel_known(channel)) {
      value = CapacityValue(0);
    }
  };
  zero_fill(CapacityChannel::INGRESS, excluded_value.ingress);
  zero_fill(CapacityChannel::EGRESS, excluded_value.egress);
  zero_fill(CapacityChannel::INTERNAL, excluded_value.internal);
  out_.capacity.excluded_by_maintenance = excluded_value;

  CapacityVector available;
  const Status subtracted =
      subtract_capacity(out_.capacity.total, out_.capacity.excluded_by_maintenance, available);
  if (is_ok(subtracted)) {
    out_.capacity.available = available;
  } else {
    out_.capacity.available = CapacityVector::all_unknown();
    out_.capacity.factors.add("available", site_fabric::to_string(subtracted));
  }

  // The closing identity, checked rather than asserted.
  bool closes = is_ok(subtracted);
  if (total_aggregate.overflowed() || excluded_aggregate.overflowed() ||
      available_aggregate.overflowed() || indeterminate_aggregate.overflowed()) {
    closes = false;
    out_.capacity.factors.add("overflow", "true");
  }
  if (!capacity_leq(out_.capacity.excluded_by_maintenance, out_.capacity.total)) {
    closes = false;
    out_.capacity.factors.add("excluded_exceeds_total", "true");
  }
  if (!capacity_leq(out_.capacity.available, out_.capacity.total)) {
    closes = false;
    out_.capacity.factors.add("available_exceeds_total", "true");
  }
  {
    CapacityVector recombined;
    const Status first = add_capacity(recombined, out_.capacity.available);
    const Status second = is_ok(first) ? add_capacity(recombined, out_.capacity.excluded_by_maintenance)
                                       : first;
    if (!is_ok(first) || !is_ok(second)) {
      closes = false;
      out_.capacity.factors.add("recombine", "overflow");
    } else {
      const bool agrees =
          (!recombined.ingress.known || !out_.capacity.total.ingress.known ||
           recombined.ingress.bps == out_.capacity.total.ingress.bps) &&
          (!recombined.egress.known || !out_.capacity.total.egress.known ||
           recombined.egress.bps == out_.capacity.total.egress.bps) &&
          (!recombined.internal.known || !out_.capacity.total.internal.known ||
           recombined.internal.bps == out_.capacity.total.internal.bps);
      if (!agrees) {
        closes = false;
        out_.capacity.factors.add("available_plus_excluded", "does_not_equal_total");
      }
    }
  }
  // An independent recomputation of available from the non-excluded entries
  // must agree with the subtraction. Two different routes to one number.
  {
    const CapacityVector recomputed_available = available_aggregate.total();
    const auto disagrees = [](const CapacityValue& left, const CapacityValue& right) {
      if (left.known != right.known) {
        return true;
      }
      return left.known && left.bps != right.bps;
    };
    if (disagrees(recomputed_available.ingress, out_.capacity.available.ingress) ||
        disagrees(recomputed_available.egress, out_.capacity.available.egress) ||
        disagrees(recomputed_available.internal, out_.capacity.available.internal)) {
      closes = false;
      out_.capacity.factors.add("available", "does_not_match_sum_of_entries");
    }
  }
  out_.capacity.closes_exactly = closes;

  const Status total_status = total_aggregate.status();
  if (!is_ok(total_status)) {
    out_.capacity.status = total_status;
    out_.capacity.factors.add("aggregate", site_fabric::to_string(total_status));
  } else {
    out_.capacity.status = Status::OK;
  }
  out_.any_unknown_capacity = out_.capacity.status == Status::CAPACITY_UNKNOWN;
  return Status::OK;
}

void Composer::build_connectivity() {
  ConnectivitySummary summary;
  for (const auto& [key, record] : resources_) {
    if (key.kind == ResourceKind::SHARED_LINK && record.link != nullptr) {
      summary.shared_links += 1;
      if (record.link->external) {
        summary.external_links += 1;
      } else {
        summary.internal_links += 1;
      }
      if (record.status != Status::OK) {
        ++summary.shared_links_unknown;
        summary.unknown_state.push_back(key);
      } else {
        switch (record.link->state) {
          case ConnectivityState::UP:
            summary.shared_links_up += 1;
            break;
          case ConnectivityState::DOWN:
            summary.shared_links_down += 1;
            break;
          case ConnectivityState::DEGRADED:
            summary.shared_links_degraded += 1;
            break;
          default:
            ++summary.shared_links_unknown;
            summary.unknown_state.push_back(key);
            break;
        }
      }
      summary.sources.insert(record.sources());
    } else if (key.kind == ResourceKind::GATEWAY && record.gateway != nullptr) {
      summary.gateways += 1;
      if (record.status != Status::OK) {
        ++summary.gateways_unknown;
        summary.unknown_state.push_back(key);
      } else {
        switch (record.gateway->state) {
          case ConnectivityState::UP:
            summary.gateways_up += 1;
            break;
          case ConnectivityState::DOWN:
            summary.gateways_down += 1;
            break;
          case ConnectivityState::DEGRADED:
            summary.gateways_degraded += 1;
            break;
          default:
            ++summary.gateways_unknown;
            summary.unknown_state.push_back(key);
            break;
        }
      }
      summary.sources.insert(record.sources());
    }
  }
  std::sort(summary.unknown_state.begin(), summary.unknown_state.end());
  summary.unknown_state.erase(std::unique(summary.unknown_state.begin(), summary.unknown_state.end()),
                              summary.unknown_state.end());

  const std::uint64_t unknown_total = summary.shared_links_unknown + summary.gateways_unknown;
  const std::uint64_t down_total = summary.shared_links_down + summary.gateways_down;
  if (summary.shared_links == 0 && summary.gateways == 0) {
    summary.status = Status::CONNECTIVITY_UNKNOWN;
  } else if (unknown_total != 0) {
    summary.status = Status::CONNECTIVITY_UNKNOWN;
  } else if (down_total != 0) {
    summary.status = Status::CONNECTIVITY_DOWN;
  } else if (summary.shared_links_degraded + summary.gateways_degraded != 0) {
    summary.status = Status::CONNECTIVITY_DEGRADED;
  } else if (summary.shared_links_up == 0 && summary.gateways_up == 0) {
    summary.status = Status::CONNECTIVITY_UNKNOWN;
  } else {
    summary.status = Status::OK;
  }
  out_.connectivity = std::move(summary);
}

Status Composer::build_obligations() {
  std::set<FailureDomainId> populated_domains;
  for (const auto& node : out_.failure_domains.nodes) {
    for (const auto& member : node.members) {
      if (resources_.find(member) != resources_.end()) {
        populated_domains.insert(node.id);
        break;
      }
    }
  }

  for (const auto& [key, record] : resources_) {
    if (key.kind != ResourceKind::OBLIGATION || record.obligation == nullptr) {
      continue;
    }
    ObligationVerdict verdict;
    verdict.id = record.obligation->id;
    verdict.kind = record.obligation->kind;
    verdict.required = record.obligation->required;
    verdict.sources = record.sources();

    if (record.status != Status::OK) {
      verdict.status = Status::OBLIGATION_INDETERMINATE;
      verdict.observed_known = false;
      verdict.factors.add("attribution", site_fabric::to_string(record.status));
      out_.obligations.push_back(std::move(verdict));
      continue;
    }

    const ProtectedObligation& obligation = *record.obligation;

    if (obligation_is_count(obligation.kind)) {
      std::uint64_t observed = 0;
      bool any_unknown = false;

      if (obligation.kind == ObligationKind::MIN_UP_SHARED_LINKS ||
          obligation.kind == ObligationKind::MIN_UP_GATEWAYS) {
        const ResourceKind wanted = obligation.kind == ObligationKind::MIN_UP_SHARED_LINKS
                                        ? ResourceKind::SHARED_LINK
                                        : ResourceKind::GATEWAY;
        for (const auto& [resource_key, resource_record] : resources_) {
          if (resource_key.kind != wanted) {
            continue;
          }
          if (obligation.scope_resource.has_value() &&
              !(*obligation.scope_resource == resource_key)) {
            continue;
          }
          if (resource_record.status != Status::OK) {
            any_unknown = true;
            continue;
          }
          const ConnectivityState state = wanted == ResourceKind::SHARED_LINK
                                              ? resource_record.link->state
                                              : resource_record.gateway->state;
          if (state == ConnectivityState::UP) {
            ++observed;
          } else if (state == ConnectivityState::UNKNOWN) {
            any_unknown = true;
          }
        }
      } else if (obligation.kind == ObligationKind::MIN_DISTINCT_FAILURE_DOMAINS) {
        observed = populated_domains.size();
        if (obligation.scope_domain.has_value()) {
          observed = 0;
          const FailureDomainNode* scope =
              out_.failure_domains.find(*obligation.scope_domain);
          if (scope == nullptr) {
            any_unknown = true;
          } else {
            for (const auto& node : out_.failure_domains.nodes) {
              const bool inside = node.id == scope->id ||
                                  std::find(node.ancestors.begin(), node.ancestors.end(),
                                            scope->id) != node.ancestors.end();
              if (inside && populated_domains.count(node.id) != 0) {
                ++observed;
              }
            }
          }
        }
        if (!out_.failure_domains.cyclic.empty() || !out_.failure_domains.dangling.empty()) {
          any_unknown = true;
        }
      } else {
        // DOMAIN_SURVIVABILITY: how many distinct descendant domains of the
        // scope still hold attributed resources.
        const FailureDomainNode* scope =
            obligation.scope_domain.has_value()
                ? out_.failure_domains.find(*obligation.scope_domain)
                : nullptr;
        if (scope == nullptr) {
          any_unknown = true;
        } else {
          for (const auto& node : out_.failure_domains.nodes) {
            if (node.id == scope->id) {
              continue;
            }
            const bool inside = std::find(node.ancestors.begin(), node.ancestors.end(),
                                          scope->id) != node.ancestors.end();
            if (inside && populated_domains.count(node.id) != 0) {
              ++observed;
            }
          }
          if (std::find(out_.failure_domains.cyclic.begin(), out_.failure_domains.cyclic.end(),
                        scope->id) != out_.failure_domains.cyclic.end()) {
            any_unknown = true;
          }
        }
      }

      verdict.observed = observed;
      if (observed >= obligation.required) {
        verdict.observed_known = true;
        verdict.status = Status::OK;
      } else if (any_unknown) {
        verdict.observed_known = false;
        verdict.status = Status::OBLIGATION_INDETERMINATE;
        verdict.factors.add("unknown_members", "true");
      } else {
        verdict.observed_known = true;
        verdict.status = Status::OBLIGATION_VIOLATED;
      }
      out_.obligations.push_back(std::move(verdict));
      continue;
    }

    const CapacityChannel channel = obligation_channel(obligation.kind);
    if (channel == CapacityChannel::UNKNOWN) {
      verdict.status = Status::OBLIGATION_INDETERMINATE;
      verdict.observed_known = false;
      verdict.factors.add("kind", "unsupported");
      out_.obligations.push_back(std::move(verdict));
      continue;
    }

    const CapacityValue value = channel_value(out_.capacity.available, channel);
    if (!value.known) {
      verdict.status = Status::OBLIGATION_INDETERMINATE;
      verdict.observed_known = false;
      verdict.factors.add("channel", site_fabric::to_string(channel));
      verdict.factors.add("available", "UNKNOWN");
      out_.obligations.push_back(std::move(verdict));
      continue;
    }
    verdict.observed = value.bps;
    verdict.observed_known = true;
    verdict.status = value.bps >= obligation.required ? Status::OK : Status::OBLIGATION_VIOLATED;
    out_.obligations.push_back(std::move(verdict));
  }

  std::sort(out_.obligations.begin(), out_.obligations.end(),
            [](const ObligationVerdict& left, const ObligationVerdict& right) {
              return left.id < right.id;
            });
  return Status::OK;
}

void Composer::build_membership() {
  std::set<MemberDomainKey> reported;
  for (const auto& [domain, admission] : admissions_) {
    reported.insert(domain);
    if (admission.lifecycle == MemberLifecycle::REVOKED) {
      add_deficit(out_, Status::LEASE_REVOKED, domain, "authority revoked");
    } else if (admission.lifecycle == MemberLifecycle::RETIRED) {
      add_deficit(out_, Status::ALREADY_RETIRED, domain, "retired");
    }
  }

  for (const auto& expected : expectation_.members) {
    const auto found = admissions_.find(expected.domain);
    if (found == admissions_.end()) {
      MemberDomainState state;
      state.domain = expected.domain;
      state.lifecycle = MemberLifecycle::ABSENT;
      state.status = Status::MEMBERSHIP_MISSING;
      state.expected = true;
      state.expectation_pinned = expected.pinned;
      state.factors.add("expected", "true");
      state.factors.add("reported", "false");
      out_.members.push_back(std::move(state));
      add_deficit(out_, Status::MEMBERSHIP_MISSING, expected.domain, "no publication received");
      continue;
    }
    const Admission& admission = found->second;
    switch (admission.lifecycle) {
      case MemberLifecycle::CURRENT:
        break;
      case MemberLifecycle::REPORTED:
        add_deficit(out_, admission.status, expected.domain, "declaration not authoritative");
        break;
      case MemberLifecycle::STALE:
        add_deficit(out_, Status::EVIDENCE_STALE, expected.domain, "evidence expired");
        break;
      case MemberLifecycle::CONFLICTING:
        add_deficit(out_, admission.status, expected.domain, "declaration conflicts");
        break;
      case MemberLifecycle::INCOMPATIBLE:
        add_deficit(out_, admission.status, expected.domain, "declaration incompatible");
        break;
      case MemberLifecycle::RETIRED:
        add_deficit(out_, Status::ALREADY_RETIRED, expected.domain, "retired");
        break;
      case MemberLifecycle::REVOKED:
        add_deficit(out_, Status::LEASE_REVOKED, expected.domain, "authority revoked");
        break;
      default:
        add_deficit(out_, Status::UNKNOWN, expected.domain, "lifecycle unknown");
        break;
    }
  }

  std::sort(out_.members.begin(), out_.members.end(),
            [](const MemberDomainState& left, const MemberDomainState& right) {
              return left.domain < right.domain;
            });
  out_.members.erase(std::unique(out_.members.begin(), out_.members.end(),
                                 [](const MemberDomainState& left, const MemberDomainState& right) {
                                   return left.domain == right.domain;
                                 }),
                     out_.members.end());

  out_.membership_complete = true;
  out_.all_evidence_fresh = true;
  if (expectation_.members.empty()) {
    out_.membership_complete = false;
    out_.factors.add("expectation", "empty");
  }
  for (const auto& expected : expectation_.members) {
    if (!expected.required) {
      continue;
    }
    const MemberDomainState* state = out_.find_member(expected.domain);
    if (state == nullptr || state->lifecycle != MemberLifecycle::CURRENT) {
      out_.membership_complete = false;
    }
  }
  for (const auto& member : out_.members) {
    if (member.evidence_state != EvidenceState::FRESH) {
      out_.all_evidence_fresh = false;
    }
  }

  // A member whose declaration conflicts is a site-level conflict. A deficit
  // says something is missing; a conflict says two sources disagree, and the
  // two must not be reported as the same thing.
  for (const auto& member : out_.members) {
    if (member.lifecycle != MemberLifecycle::CONFLICTING) {
      continue;
    }
    SourceSet parties;
    if (member.source.valid()) {
      parties.insert(member.source);
    }
    add_conflict(out_, member.status, ResourceKey{}, parties, {member.factors.join(",")});
  }

  out_.any_conflict = !out_.conflicts.empty();
  std::sort(out_.conflicts.begin(), out_.conflicts.end(),
            [](const SiteConflict& left, const SiteConflict& right) {
              if (left.code != right.code) {
                return static_cast<int>(left.code) < static_cast<int>(right.code);
              }
              return left.subject < right.subject;
            });
  std::sort(out_.deficits.begin(), out_.deficits.end(),
            [](const SiteDeficit& left, const SiteDeficit& right) {
              if (left.domain != right.domain) {
                return left.domain < right.domain;
              }
              return static_cast<int>(left.code) < static_cast<int>(right.code);
            });
}

void Composer::build_lifecycle() {
  bool any_required_missing = false;
  bool all_required_absent = !expectation_.members.empty();
  bool any_non_authoritative = false;
  bool any_obligation_violated = false;
  bool any_obligation_indeterminate = false;
  bool any_down = false;
  bool any_degraded = false;

  for (const auto& member : out_.members) {
    if (member.lifecycle != MemberLifecycle::CURRENT) {
      any_non_authoritative = true;
      if (member.lifecycle != MemberLifecycle::ABSENT) {
        all_required_absent = false;
      }
    }
  }
  for (const auto& expected : expectation_.members) {
    if (!expected.required) {
      continue;
    }
    const MemberDomainState* state = out_.find_member(expected.domain);
    if (state == nullptr || state->lifecycle != MemberLifecycle::CURRENT) {
      any_required_missing = true;
      if (state != nullptr && state->lifecycle != MemberLifecycle::ABSENT) {
        all_required_absent = false;
      }
    } else {
      all_required_absent = false;
    }
  }
  if (expectation_.members.empty()) {
    all_required_absent = false;
    any_required_missing = true;
  }

  for (const auto& verdict : out_.obligations) {
    if (verdict.status == Status::OBLIGATION_VIOLATED) {
      any_obligation_violated = true;
    } else if (verdict.status == Status::OBLIGATION_INDETERMINATE) {
      any_obligation_indeterminate = true;
    }
  }
  if (out_.connectivity.shared_links_down != 0 || out_.connectivity.gateways_down != 0) {
    any_down = true;
  }
  if (out_.connectivity.shared_links_degraded != 0 || out_.connectivity.gateways_degraded != 0) {
    any_degraded = true;
  }

  const bool capacity_indeterminate = out_.capacity.status == Status::CAPACITY_UNKNOWN;
  const bool forest_indeterminate =
      out_.failure_domains.status == Status::FAILURE_DOMAIN_DANGLING_PARENT ||
      out_.failure_domains.status == Status::NO_AUTHORITATIVE_SOURCE ||
      !out_.failure_domains.cyclic.empty();
  const bool connectivity_indeterminate =
      out_.connectivity.status == Status::CONNECTIVITY_UNKNOWN;
  const bool maintenance_indeterminate = maintenance_indeterminate_;

  if (out_.any_conflict) {
    out_.lifecycle = SiteLifecycle::CONFLICTING;
  } else if (all_required_absent) {
    out_.lifecycle = SiteLifecycle::PARTITIONED;
  } else if (any_required_missing) {
    out_.lifecycle = SiteLifecycle::INCOMPLETE;
  } else if (capacity_indeterminate || forest_indeterminate || connectivity_indeterminate ||
             any_obligation_indeterminate || any_non_authoritative ||
             maintenance_indeterminate) {
    out_.lifecycle = SiteLifecycle::INDETERMINATE;
  } else if (any_obligation_violated || any_down || any_degraded) {
    out_.lifecycle = SiteLifecycle::DEGRADED;
  } else {
    out_.lifecycle = SiteLifecycle::CURRENT;
  }

  switch (out_.lifecycle) {
    case SiteLifecycle::CURRENT:
      out_.status = Status::CURRENT;
      break;
    case SiteLifecycle::DEGRADED:
      out_.status = Status::DEGRADED;
      break;
    case SiteLifecycle::PARTITIONED:
      out_.status = Status::PARTITIONED;
      break;
    case SiteLifecycle::INCOMPLETE:
      out_.status = Status::INCOMPLETE;
      break;
    case SiteLifecycle::CONFLICTING:
      out_.status = Status::CONFLICTING;
      break;
    case SiteLifecycle::INDETERMINATE:
      out_.status = Status::INDETERMINATE;
      break;
    default:
      out_.status = Status::NOT_STARTED;
      break;
  }

  out_.factors.add("lifecycle", site_fabric::to_string(out_.lifecycle));
  out_.factors.add("members", std::to_string(out_.members.size()));
  out_.factors.add("conflicts", std::to_string(out_.conflicts.size()));
  out_.factors.add("deficits", std::to_string(out_.deficits.size()));
  out_.factors.add("capacity", site_fabric::to_string(out_.capacity.status));
}

void Composer::build_decisions() {
  SourceSet membership_sources;
  for (const auto& [domain, admission] : admissions_) {
    (void)domain;
    membership_sources.insert(admission.source);
  }
  add_decision(out_, DecisionKind::MEMBERSHIP_COMPLETENESS, ResourceKey{},
               out_.membership_complete ? Status::OK : Status::INCOMPLETE, membership_sources,
               Factors{});
  add_decision(out_, DecisionKind::SITE_EPOCH, ResourceKey{}, Status::OK, membership_sources,
               Factors{});

  SourceSet rack_sources;
  SourceSet link_sources;
  SourceSet accounting_sources;
  SourceSet forest_sources;
  SourceSet capacity_sources;

  for (const auto& attribution : out_.ownership) {
    add_decision(out_, DecisionKind::OWNERSHIP_MAP, attribution.key, attribution.status,
                 attribution.attestors, attribution.factors);
    switch (attribution.key.kind) {
      case ResourceKind::RACK:
        rack_sources.insert(attribution.attestors);
        break;
      case ResourceKind::SHARED_LINK:
      case ResourceKind::GATEWAY:
        link_sources.insert(attribution.attestors);
        accounting_sources.insert(attribution.attestors);
        break;
      case ResourceKind::CAPACITY_POOL:
        accounting_sources.insert(attribution.attestors);
        break;
      case ResourceKind::FAILURE_DOMAIN:
        forest_sources.insert(attribution.attestors);
        break;
      default:
        break;
    }
  }

  for (const auto& entry : out_.capacity.entries) {
    if (entry.counted_in_total) {
      capacity_sources.insert(entry.attestors);
    }
  }

  add_decision(out_, DecisionKind::RACK_TOPOLOGY, ResourceKey{}, Status::OK, rack_sources,
               Factors{});
  add_decision(out_, DecisionKind::FAILURE_DOMAIN_FOREST, ResourceKey{},
               out_.failure_domains.status, forest_sources, out_.failure_domains.factors);
  add_decision(out_, DecisionKind::CONNECTIVITY_SUMMARY, ResourceKey{}, out_.connectivity.status,
               link_sources, Factors{});
  add_decision(out_, DecisionKind::SHARED_RESOURCE_ACCOUNTING, ResourceKey{}, Status::OK,
               accounting_sources, Factors{});
  add_decision(out_, DecisionKind::CAPACITY_TOTAL, ResourceKey{}, out_.capacity.status,
               capacity_sources, out_.capacity.factors);
  add_decision(out_, DecisionKind::CAPACITY_AVAILABLE, ResourceKey{}, out_.capacity.status,
               capacity_sources, out_.capacity.factors);

  SourceSet maintenance_sources;
  for (const auto& effect : out_.maintenance) {
    const auto found = resources_.find(ResourceKey::maintenance_zone(effect.id.value()));
    SourceSet sources;
    if (found != resources_.end()) {
      sources.insert(found->second.sources());
    }
    for (const auto& key : effect.excluded) {
      const auto resource = resources_.find(key);
      if (resource != resources_.end()) {
        sources.insert(resource->second.sources());
      }
    }
    maintenance_sources.insert(sources);
    add_decision(out_, DecisionKind::MAINTENANCE_EXCLUSION,
                 ResourceKey::maintenance_zone(effect.id.value()), effect.status, sources,
                 effect.factors);
  }

  for (const auto& verdict : out_.obligations) {
    add_decision(out_, DecisionKind::OBLIGATION_VERDICT, ResourceKey::obligation(verdict.id.value()),
                 verdict.status, verdict.sources, verdict.factors);
  }

  SourceSet lifecycle_sources;
  for (const auto& decision : out_.decisions) {
    lifecycle_sources.insert(decision.sources);
  }
  add_decision(out_, DecisionKind::SITE_LIFECYCLE, ResourceKey{}, out_.status, lifecycle_sources,
               Factors{});

  std::sort(out_.decisions.begin(), out_.decisions.end(),
            [](const SiteDecision& left, const SiteDecision& right) {
              return left.id < right.id;
            });
}

Status Composer::run() {
  const Status prepared = prepare();
  if (!is_ok(prepared)) {
    return prepared;
  }
  const Status admitted = admit_members();
  if (!is_ok(admitted)) {
    return admitted;
  }
  const Status attributed = attribute_resources();
  if (!is_ok(attributed)) {
    return attributed;
  }
  const Status forest = build_failure_domains();
  if (!is_ok(forest)) {
    return forest;
  }
  const Status maintenance = build_maintenance();
  if (!is_ok(maintenance)) {
    return maintenance;
  }
  build_connectivity();
  const Status obligations = build_obligations();
  if (!is_ok(obligations)) {
    return obligations;
  }
  build_membership();
  build_lifecycle();
  build_decisions();
  return Status::OK;
}

}  // namespace

Status SiteComposer::compose(const CompositionInput& input, ComposedSite& out) const {
  ComposedSite local;
  Composer composer(input, local);
  const Status status = composer.run();
  if (!is_ok(status)) {
    last_work_units_ = 0;
    return status;
  }
  out = std::move(local);
  last_work_units_ = out.members.size() + out.ownership.size() + out.capacity.entries.size() +
                     out.decisions.size();
  return Status::COMPOSED;
}

// ---------------------------------------------------------------------------
// Invalidation
// ---------------------------------------------------------------------------

std::string InvalidationReport::to_string() const {
  std::string out = "changed=" + changed.to_string();
  out += " invalidated=" + std::to_string(invalidated.size());
  out += " retained=" + std::to_string(retained.size());
  out += " domains=" + std::to_string(affected_domains.size());
  out += " resources=" + std::to_string(affected_resources.size());
  return out;
}

bool InvalidationReport::invalidates_everything() const noexcept {
  for (const auto& id : invalidated) {
    if (id.kind == DecisionKind::SITE_LIFECYCLE) {
      return true;
    }
  }
  return false;
}

InvalidationReport invalidate_for_source(const ComposedSite& site, const SourceRef& changed) {
  InvalidationReport report;
  report.domain = changed.domain;
  report.changed = changed;

  for (const auto& decision : site.decisions) {
    if (decision.depends_on(changed)) {
      report.invalidated.push_back(decision.id);
      if (std::find(report.recomposition_required.begin(), report.recomposition_required.end(),
                    decision.id.kind) == report.recomposition_required.end()) {
        report.recomposition_required.push_back(decision.id.kind);
      }
    } else {
      report.retained.push_back(decision.id);
    }
  }

  if (const MemberDomainState* member = site.find_member(changed.domain); member != nullptr) {
    if (member->source == changed) {
      report.affected_domains.push_back(changed.domain);
    }
  }

  for (const auto& attribution : site.ownership) {
    if (attribution.attestors.contains(changed)) {
      report.affected_resources.push_back(attribution.key);
    }
  }

  std::sort(report.recomposition_required.begin(), report.recomposition_required.end(),
            [](DecisionKind left, DecisionKind right) {
              return static_cast<int>(left) < static_cast<int>(right);
            });
  std::sort(report.invalidated.begin(), report.invalidated.end());
  std::sort(report.retained.begin(), report.retained.end());
  std::sort(report.affected_resources.begin(), report.affected_resources.end());
  std::sort(report.affected_domains.begin(), report.affected_domains.end());
  return report;
}

InvalidationReport invalidate_for_domain(const ComposedSite& site,
                                         const MemberDomainKey& domain) {
  InvalidationReport report;
  report.domain = domain;

  for (const auto& decision : site.decisions) {
    if (decision.depends_on_domain(domain)) {
      report.invalidated.push_back(decision.id);
      if (std::find(report.recomposition_required.begin(), report.recomposition_required.end(),
                    decision.id.kind) == report.recomposition_required.end()) {
        report.recomposition_required.push_back(decision.id.kind);
      }
    } else {
      report.retained.push_back(decision.id);
    }
  }
  if (const MemberDomainState* member = site.find_member(domain); member != nullptr) {
    report.changed = member->source;
    report.affected_domains.push_back(domain);
  }
  for (const auto& attribution : site.ownership) {
    if (attribution.attestors.contains_domain(domain)) {
      report.affected_resources.push_back(attribution.key);
    }
  }
  std::sort(report.recomposition_required.begin(), report.recomposition_required.end(),
            [](DecisionKind left, DecisionKind right) {
              return static_cast<int>(left) < static_cast<int>(right);
            });
  std::sort(report.invalidated.begin(), report.invalidated.end());
  std::sort(report.retained.begin(), report.retained.end());
  std::sort(report.affected_resources.begin(), report.affected_resources.end());
  std::sort(report.affected_domains.begin(), report.affected_domains.end());
  return report;
}

// ---------------------------------------------------------------------------
// Summaries
// ---------------------------------------------------------------------------

std::string summarize(const ComposedSite& site) {
  std::string out = site.site.value();
  out += " epoch=" + site.epoch.to_string();
  out += " gen=" + site.generation.to_string();
  out += " " + std::string(site_fabric::to_string(site.lifecycle));
  out += " members=" + std::to_string(site.members.size());
  out += " conflicts=" + std::to_string(site.conflicts.size());
  out += " deficits=" + std::to_string(site.deficits.size());
  out += " " + site.capacity.to_string();
  return out;
}

std::string explain(const ComposedSite& site) {
  std::string out = summarize(site);
  out += "\n  capacity: " + site.capacity.to_string();
  out += "\n  connectivity: " + site.connectivity.to_string();
  out += "\n  failure domains: " + std::to_string(site.failure_domains.nodes.size()) +
         " nodes, status " + site_fabric::to_string(site.failure_domains.status);
  out += "\n  decisions: " + std::to_string(site.decisions.size());
  for (const auto& conflict : site.conflicts) {
    out += "\n  conflict: " + conflict.to_string();
  }
  for (const auto& deficit : site.deficits) {
    out += "\n  deficit: " + deficit.to_string();
  }
  for (const auto& verdict : site.obligations) {
    out += "\n  obligation: " + verdict.to_string();
  }
  for (const auto& factor : site.factors.items()) {
    out += "\n  factor: " + factor;
  }
  return out;
}

}  // namespace site_fabric
