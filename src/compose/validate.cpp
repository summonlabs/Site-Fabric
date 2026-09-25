// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Structural validation and canonicalisation.
//
// Validation runs before interpretation. A declaration is canonicalised first:
// every list is sorted into one order and exact duplicates are collapsed. Only
// then is it read. A list that holds two different records under one identity
// is refused at this stage, so no later stage has to decide which of them it
// meant.

#include <algorithm>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "core/digest.hpp"
#include "site_fabric/site_fabric.hpp"

namespace site_fabric {
namespace {

[[nodiscard]] Status require_bound(std::size_t size, std::size_t bound, const char* label,
                                   Factors& factors) {
  if (size > bound) {
    factors.add(label, std::to_string(size));
    factors.add("bound", std::to_string(bound));
    return Status::LIMIT_EXCEEDED;
  }
  return Status::OK;
}

/// Sorts a claim list by its canonical key and collapses exact duplicates.
///
/// Two different contents under one key is a conflict, not something to
/// resolve: the caller is told which key disagreed and with how many variants.
template <class T, class KeyFn, class DigestFn>
[[nodiscard]] Status canonicalize_list(std::vector<T>& items, std::size_t bound,
                                       const char* label, KeyFn key_of, DigestFn digest_of,
                                       Factors& factors) {
  const Status bounded = require_bound(items.size(), bound, label, factors);
  if (!is_ok(bounded)) {
    return bounded;
  }

  std::stable_sort(items.begin(), items.end(), [&key_of](const T& left, const T& right) {
    return key_of(left) < key_of(right);
  });

  std::vector<T> collapsed;
  collapsed.reserve(items.size());
  std::size_t index = 0;
  while (index < items.size()) {
    std::size_t end = index + 1;
    const std::string key = key_of(items[index]);
    while (end < items.size() && key_of(items[end]) == key) {
      ++end;
    }
    const Digest first = digest_of(items[index]);
    bool agreed = true;
    for (std::size_t probe = index + 1; probe < end; ++probe) {
      if (!(digest_of(items[probe]) == first)) {
        agreed = false;
        break;
      }
    }
    if (!agreed) {
      factors.add("duplicate_identity", key);
      factors.add("variants", std::to_string(end - index));
      return Status::CONFLICTING;
    }
    collapsed.push_back(items[index]);
    index = end;
  }
  items.swap(collapsed);
  return Status::OK;
}

[[nodiscard]] bool record_digest_agrees(const Digest& declared, const Digest& computed) {
  return declared.is_zero() || declared == computed;
}

[[nodiscard]] Status check_record_digest(const Digest& declared, const Digest& computed,
                                         const char* label, Factors& factors) {
  if (record_digest_agrees(declared, computed)) {
    return Status::OK;
  }
  factors.add(label, "record_digest_mismatch");
  factors.add("declared", declared.short_hex());
  factors.add("computed", computed.short_hex());
  return Status::DIGEST_MISMATCH_DECLARATION;
}

[[nodiscard]] Status validate_evidence(const Evidence& evidence, const char* label,
                                       Factors& factors, bool require_payload) {
  if (evidence.provenance == Provenance::UNKNOWN) {
    factors.add(label, "provenance_unknown");
    return Status::EVIDENCE_MISSING;
  }
  if (evidence.observed_at_ms <= 0) {
    factors.add(label, "observed_at_not_positive");
    return Status::INVALID;
  }
  if (evidence.ttl_ms == 0 || evidence.ttl_ms > limits::kMaxEvidenceTtlMs) {
    factors.add(label, "ttl_out_of_range");
    factors.add("ttl_ms", std::to_string(evidence.ttl_ms));
    return Status::INVALID;
  }
  if (require_payload && evidence.payload_digest.is_zero()) {
    factors.add(label, "payload_digest_absent");
    return Status::INVALID;
  }
  return Status::OK;
}

[[nodiscard]] bool valid_capacity(const CapacityVector& vector, Factors& factors,
                                  const char* label) {
  bool ok = true;
  const auto check = [&](const CapacityValue& value, const char* channel) {
    if (value.known && value.bps > limits::kMaxCapacityBps) {
      factors.add(label, std::string(channel) + "_above_bound");
      ok = false;
    }
  };
  check(vector.ingress, "ingress");
  check(vector.egress, "egress");
  check(vector.internal, "internal");
  return ok;
}

}  // namespace

// ---------------------------------------------------------------------------
// Claims
// ---------------------------------------------------------------------------

bool FailureDomainClaim::valid() const noexcept {
  if (!id.valid() || !is_valid(domain_class) || generation.is_unset()) {
    return false;
  }
  if (parent.has_value() && !parent->valid()) {
    return false;
  }
  if (parent.has_value() && *parent == id) {
    return false;
  }
  for (const auto& member : members) {
    if (!member.valid()) {
      return false;
    }
  }
  return true;
}

bool MaintenanceZone::valid() const noexcept {
  if (!id.valid() || generation.is_unset()) {
    return false;
  }
  if (window_end_ms < window_start_ms) {
    return false;
  }
  for (const auto& domain : domains) {
    if (!domain.valid()) {
      return false;
    }
  }
  for (const auto& resource : resources) {
    if (!resource.valid()) {
      return false;
    }
  }
  return true;
}

bool MaintenanceZone::window_contains(std::int64_t now_ms) const noexcept {
  if (window_start_ms == 0 && window_end_ms == 0) {
    return true;
  }
  return now_ms >= window_start_ms && now_ms <= window_end_ms;
}

bool ProtectedObligation::valid() const noexcept {
  if (!id.valid() || !is_valid(kind) || generation.is_unset()) {
    return false;
  }
  if (scope_domain.has_value() && !scope_domain->valid()) {
    return false;
  }
  if (scope_resource.has_value() && !scope_resource->valid()) {
    return false;
  }
  const std::uint64_t bound =
      obligation_is_count(kind) ? limits::kMaxCountValue : limits::kMaxCapacityBps;
  return required <= bound;
}

// ---------------------------------------------------------------------------
// Failure-domain forest
// ---------------------------------------------------------------------------

const FailureDomainNode* FailureDomainForest::find(const FailureDomainId& id) const noexcept {
  for (const auto& node : nodes) {
    if (node.id == id) {
      return &node;
    }
  }
  return nullptr;
}

bool FailureDomainNode::ancestor_of(const FailureDomainId& other) const noexcept {
  return std::find(ancestors.begin(), ancestors.end(), other) != ancestors.end();
}

std::string FailureDomainNode::to_string() const {
  std::string out = id.value();
  out += "(";
  out += site_fabric::to_string(domain_class);
  out += ",depth=" + std::to_string(depth);
  out += ",members=" + std::to_string(members.size());
  out += ",children=" + std::to_string(children.size());
  out += ",";
  out += site_fabric::to_string(status);
  out += ")";
  return out;
}

void FailureDomainForest::resolve_ancestry() {
  std::map<FailureDomainId, std::size_t> index_of;
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    index_of[nodes[index].id] = index;
  }

  const FailureDomainId* cyclic_marker = nullptr;
  (void)cyclic_marker;

  for (std::size_t index = 0; index < nodes.size(); ++index) {
    FailureDomainNode& node = nodes[index];
    node.ancestors.clear();
    node.depth = 0;

    std::vector<FailureDomainId> seen;
    const FailureDomainNode* cursor = &node;
    std::uint32_t depth = 0;
    bool cycle = false;
    while (cursor->parent.has_value()) {
      const FailureDomainId parent_id = *cursor->parent;
      if (std::find(seen.begin(), seen.end(), parent_id) != seen.end() || parent_id == node.id) {
        cycle = true;
        break;
      }
      if (depth >= limits::kMaxFailureDomainDepth) {
        cycle = true;
        break;
      }
      seen.push_back(parent_id);
      const auto found = index_of.find(parent_id);
      if (found == index_of.end()) {
        break;
      }
      cursor = &nodes[found->second];
      node.ancestors.push_back(parent_id);
      ++depth;
    }

    if (cycle) {
      node.ancestors.clear();
      node.depth = 0;
      node.status = Status::FAILURE_DOMAIN_CYCLE;
      if (std::find(cyclic.begin(), cyclic.end(), node.id) == cyclic.end()) {
        cyclic.push_back(node.id);
      }
      continue;
    }
    node.depth = depth;
  }

  // Children lists follow from the resolved parents.
  for (auto& node : nodes) {
    node.children.clear();
  }
  for (const auto& node : nodes) {
    if (!node.parent.has_value()) {
      continue;
    }
    const auto found = index_of.find(*node.parent);
    if (found == index_of.end()) {
      continue;
    }
    FailureDomainNode& parent = nodes[found->second];
    if (std::find(parent.children.begin(), parent.children.end(), node.id) ==
        parent.children.end()) {
      parent.children.push_back(node.id);
    }
  }
  for (auto& node : nodes) {
    std::sort(node.children.begin(), node.children.end());
  }

  std::sort(cyclic.begin(), cyclic.end());
  std::sort(dangling.begin(), dangling.end());
}

bool FailureDomainForest::are_separate(const ResourceKey& left, const ResourceKey& right) const {
  std::vector<FailureDomainId> left_domains;
  std::vector<FailureDomainId> right_domains;
  for (const auto& node : nodes) {
    const bool has_left = std::find(node.members.begin(), node.members.end(), left) !=
                          node.members.end();
    const bool has_right = std::find(node.members.begin(), node.members.end(), right) !=
                           node.members.end();
    if (has_left) {
      left_domains.push_back(node.id);
      for (const auto& ancestor : node.ancestors) {
        left_domains.push_back(ancestor);
      }
    }
    if (has_right) {
      right_domains.push_back(node.id);
      for (const auto& ancestor : node.ancestors) {
        right_domains.push_back(ancestor);
      }
    }
  }
  if (left_domains.empty() || right_domains.empty()) {
    return false;
  }
  for (const auto& candidate : left_domains) {
    if (std::find(right_domains.begin(), right_domains.end(), candidate) != right_domains.end()) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Declaration
// ---------------------------------------------------------------------------

std::size_t MemberDomainDeclaration::resource_claim_count() const noexcept {
  return clusters.size() + pods.size() + racks.size() + shared_links.size() + gateways.size() +
         capacity.size() + failure_domains.size() + maintenance_zones.size() + obligations.size();
}

Status MemberDomainDeclaration::canonicalize() {
  Factors factors;

  if (!domain.valid()) {
    factors.add("domain", "invalid");
    OutcomeBuilder builder(Status::INVALID);
    return builder.build().status;
  }
  if (!site.valid()) {
    return Status::INVALID;
  }
  if (generation.is_unset()) {
    return Status::INVALID;
  }
  if (!schema.compatible_with(SchemaVersion{kModelSchemaMajor, kModelSchemaMinor})) {
    return Status::SCHEMA_INCOMPATIBLE;
  }
  const Status evidence_status = validate_evidence(evidence, "evidence", factors, false);
  if (!is_ok(evidence_status)) {
    return evidence_status;
  }

  const Status list_bounds = require_bound(resource_claim_count(), limits::kMaxClaimLists,
                                           "claim_count", factors);
  if (!is_ok(list_bounds)) {
    return list_bounds;
  }

  // Every claim's own evidence is validated, not only the declaration's. A
  // claim whose evidence is missing is not silently accepted because the
  // envelope around it looked fine.
  const auto claim_evidence_ok = [&factors](const Evidence& evidence, const char* label) {
    return validate_evidence(evidence, label, factors, false);
  };

  // --- Clusters -----------------------------------------------------------
  for (auto& claim : clusters) {
    if (!claim.id.valid() || claim.generation.is_unset()) {
      return Status::MALFORMED;
    }
    const Status claim_evidence_status = claim_evidence_ok(claim.evidence, "cluster_evidence");
    if (!is_ok(claim_evidence_status)) {
      return claim_evidence_status;
    }
    std::sort(claim.pods.begin(), claim.pods.end());
    claim.pods.erase(std::unique(claim.pods.begin(), claim.pods.end()), claim.pods.end());
    std::sort(claim.racks.begin(), claim.racks.end());
    claim.racks.erase(std::unique(claim.racks.begin(), claim.racks.end()), claim.racks.end());
    const Status digest_status =
        check_record_digest(claim.record_digest, internal::claim_digest(claim), "cluster", factors);
    if (!is_ok(digest_status)) {
      return digest_status;
    }
  }
  {
    const Status status = canonicalize_list<ClusterClaim>(
        clusters, limits::kMaxClaimLists, "clusters",
        [](const ClusterClaim& claim) { return claim.id.value(); },
        [](const ClusterClaim& claim) { return internal::claim_digest(claim); }, factors);
    if (!is_ok(status)) {
      return status;
    }
  }

  // --- Pods ---------------------------------------------------------------
  for (auto& claim : pods) {
    if (!claim.id.valid() || claim.generation.is_unset()) {
      return Status::MALFORMED;
    }
    const Status claim_evidence_status = claim_evidence_ok(claim.evidence, "pod_evidence");
    if (!is_ok(claim_evidence_status)) {
      return claim_evidence_status;
    }
    std::sort(claim.racks.begin(), claim.racks.end());
    claim.racks.erase(std::unique(claim.racks.begin(), claim.racks.end()), claim.racks.end());
    const Status digest_status =
        check_record_digest(claim.record_digest, internal::claim_digest(claim), "pod", factors);
    if (!is_ok(digest_status)) {
      return digest_status;
    }
  }
  {
    const Status status = canonicalize_list<PodClaim>(
        pods, limits::kMaxClaimLists, "pods",
        [](const PodClaim& claim) { return claim.id.value(); },
        [](const PodClaim& claim) { return internal::claim_digest(claim); }, factors);
    if (!is_ok(status)) {
      return status;
    }
  }

  // --- Racks --------------------------------------------------------------
  for (const auto& claim : racks) {
    if (!claim.id.valid() || claim.generation.is_unset()) {
      return Status::MALFORMED;
    }
    const Status claim_evidence_status = claim_evidence_ok(claim.evidence, "rack_evidence");
    if (!is_ok(claim_evidence_status)) {
      return claim_evidence_status;
    }
    if (!valid_capacity(claim.local_capacity, factors, "rack_capacity")) {
      return Status::CAPACITY_OVERFLOW;
    }
    const Status digest_status =
        check_record_digest(claim.record_digest, internal::claim_digest(claim), "rack", factors);
    if (!is_ok(digest_status)) {
      return digest_status;
    }
  }
  {
    const Status status = canonicalize_list<RackClaim>(
        racks, limits::kMaxClaimLists, "racks",
        [](const RackClaim& claim) { return claim.id.value(); },
        [](const RackClaim& claim) { return internal::claim_digest(claim); }, factors);
    if (!is_ok(status)) {
      return status;
    }
  }

  // --- Shared links -------------------------------------------------------
  for (const auto& claim : shared_links) {
    if (!claim.id.valid() || claim.generation.is_unset()) {
      return Status::MALFORMED;
    }
    const Status claim_evidence_status = claim_evidence_ok(claim.evidence, "shared_link_evidence");
    if (!is_ok(claim_evidence_status)) {
      return claim_evidence_status;
    }
    if (!valid_capacity(claim.capacity, factors, "shared_link_capacity")) {
      return Status::CAPACITY_OVERFLOW;
    }
    const Status digest_status = check_record_digest(
        claim.record_digest, internal::claim_digest(claim), "shared_link", factors);
    if (!is_ok(digest_status)) {
      return digest_status;
    }
  }
  {
    const Status status = canonicalize_list<SharedLinkClaim>(
        shared_links, limits::kMaxClaimLists, "shared_links",
        [](const SharedLinkClaim& claim) { return claim.id.value(); },
        [](const SharedLinkClaim& claim) { return internal::claim_digest(claim); }, factors);
    if (!is_ok(status)) {
      return status;
    }
  }

  // --- Gateways -----------------------------------------------------------
  for (const auto& claim : gateways) {
    if (!claim.id.valid() || claim.generation.is_unset()) {
      return Status::MALFORMED;
    }
    const Status claim_evidence_status = claim_evidence_ok(claim.evidence, "gateway_evidence");
    if (!is_ok(claim_evidence_status)) {
      return claim_evidence_status;
    }
    if (!valid_capacity(claim.capacity, factors, "gateway_capacity")) {
      return Status::CAPACITY_OVERFLOW;
    }
    const Status digest_status =
        check_record_digest(claim.record_digest, internal::claim_digest(claim), "gateway", factors);
    if (!is_ok(digest_status)) {
      return digest_status;
    }
  }
  {
    const Status status = canonicalize_list<GatewayClaim>(
        gateways, limits::kMaxClaimLists, "gateways",
        [](const GatewayClaim& claim) { return claim.id.value(); },
        [](const GatewayClaim& claim) { return internal::claim_digest(claim); }, factors);
    if (!is_ok(status)) {
      return status;
    }
  }

  // --- Capacity contributions --------------------------------------------
  for (const auto& claim : capacity) {
    if (!claim.owner.valid() || !is_valid(claim.scope) || claim.generation.is_unset()) {
      return Status::MALFORMED;
    }
    const Status claim_evidence_status = claim_evidence_ok(claim.evidence, "capacity_evidence");
    if (!is_ok(claim_evidence_status)) {
      return claim_evidence_status;
    }
    if (!valid_capacity(claim.capacity, factors, "capacity")) {
      return Status::CAPACITY_OVERFLOW;
    }
    const Status digest_status = check_record_digest(
        claim.record_digest, internal::claim_digest(claim), "capacity", factors);
    if (!is_ok(digest_status)) {
      return digest_status;
    }
  }
  {
    const Status status = canonicalize_list<CapacityContribution>(
        capacity, limits::kMaxCapacityContributions, "capacity",
        [](const CapacityContribution& claim) {
          return claim.owner.to_string() + "|" + site_fabric::to_string(claim.scope);
        },
        [](const CapacityContribution& claim) { return internal::claim_digest(claim); }, factors);
    if (!is_ok(status)) {
      return status;
    }
  }

  // --- Failure domains ----------------------------------------------------
  for (auto& claim : failure_domains) {
    if (!claim.valid()) {
      return Status::MALFORMED;
    }
    const Status claim_evidence_status = claim_evidence_ok(claim.evidence, "failure_domain_evidence");
    if (!is_ok(claim_evidence_status)) {
      return claim_evidence_status;
    }
    std::sort(claim.members.begin(), claim.members.end());
    claim.members.erase(std::unique(claim.members.begin(), claim.members.end()), claim.members.end());
    if (claim.members.size() > limits::kMaxResourceMembers) {
      return Status::LIMIT_EXCEEDED;
    }
    const Status digest_status = check_record_digest(
        claim.record_digest, internal::claim_digest(claim), "failure_domain", factors);
    if (!is_ok(digest_status)) {
      return digest_status;
    }
  }
  {
    const Status status = canonicalize_list<FailureDomainClaim>(
        failure_domains, limits::kMaxFailureDomainsPerDeclaration, "failure_domains",
        [](const FailureDomainClaim& claim) { return claim.id.value(); },
        [](const FailureDomainClaim& claim) { return internal::claim_digest(claim); }, factors);
    if (!is_ok(status)) {
      return status;
    }
  }

  // --- Maintenance --------------------------------------------------------
  for (auto& claim : maintenance_zones) {
    if (!claim.valid()) {
      return Status::MALFORMED;
    }
    const Status claim_evidence_status = claim_evidence_ok(claim.evidence, "maintenance_evidence");
    if (!is_ok(claim_evidence_status)) {
      return claim_evidence_status;
    }
    std::sort(claim.domains.begin(), claim.domains.end());
    claim.domains.erase(std::unique(claim.domains.begin(), claim.domains.end()), claim.domains.end());
    std::sort(claim.resources.begin(), claim.resources.end());
    claim.resources.erase(std::unique(claim.resources.begin(), claim.resources.end()),
                          claim.resources.end());
    if (claim.domains.empty() && claim.resources.empty()) {
      factors.add("maintenance_zone", claim.id.value());
      factors.add("scope", "empty");
      return Status::INVALID;
    }
    const Status digest_status = check_record_digest(
        claim.record_digest, internal::claim_digest(claim), "maintenance_zone", factors);
    if (!is_ok(digest_status)) {
      return digest_status;
    }
  }
  {
    const Status status = canonicalize_list<MaintenanceZone>(
        maintenance_zones, limits::kMaxMaintenanceZonesPerDeclaration, "maintenance_zones",
        [](const MaintenanceZone& claim) { return claim.id.value(); },
        [](const MaintenanceZone& claim) { return internal::claim_digest(claim); }, factors);
    if (!is_ok(status)) {
      return status;
    }
  }

  // --- Obligations --------------------------------------------------------
  for (auto& claim : obligations) {
    if (!claim.valid()) {
      return Status::MALFORMED;
    }
    const Status claim_evidence_status = claim_evidence_ok(claim.evidence, "obligation_evidence");
    if (!is_ok(claim_evidence_status)) {
      return claim_evidence_status;
    }
    const Status digest_status = check_record_digest(
        claim.record_digest, internal::claim_digest(claim), "obligation", factors);
    if (!is_ok(digest_status)) {
      return digest_status;
    }
  }
  {
    const Status status = canonicalize_list<ProtectedObligation>(
        obligations, limits::kMaxObligationsPerDeclaration, "obligations",
        [](const ProtectedObligation& claim) { return claim.id.value(); },
        [](const ProtectedObligation& claim) { return internal::claim_digest(claim); }, factors);
    if (!is_ok(status)) {
      return status;
    }
  }

  return Status::OK;
}

Digest MemberDomainDeclaration::compute_digest() const {
  return internal::declaration_content_digest(*this);
}

Status MemberDomainDeclaration::seal() {
  const Status status = canonicalize();
  if (!is_ok(status)) {
    return status;
  }
  digest = compute_digest();
  return Status::OK;
}

Status MemberDomainDeclaration::validate() const {
  MemberDomainDeclaration copy = *this;
  return copy.canonicalize();
}

std::string MemberDomainDeclaration::to_string() const {
  return domain.to_string() + "@" + generation.to_string() + "#" + digest.short_hex() +
         " claims=" + std::to_string(resource_claim_count());
}

// ---------------------------------------------------------------------------
// Expectation
// ---------------------------------------------------------------------------

Status SiteExpectation::canonicalize() {
  if (!site.valid()) {
    return Status::INVALID;
  }
  if (!schema.compatible_with(SchemaVersion{kModelSchemaMajor, kModelSchemaMinor})) {
    return Status::SCHEMA_INCOMPATIBLE;
  }
  if (members.size() > limits::kMaxExpectedMembers) {
    return Status::LIMIT_EXCEEDED;
  }
  std::stable_sort(members.begin(), members.end(),
                   [](const ExpectedMember& left, const ExpectedMember& right) {
                     return left.domain < right.domain;
                   });
  for (std::size_t index = 1; index < members.size(); ++index) {
    if (members[index - 1].domain == members[index].domain) {
      return Status::CONFLICTING;
    }
  }
  for (const auto& member : members) {
    if (!member.domain.valid()) {
      return Status::MALFORMED;
    }
    if (member.pinned && (member.generation.is_unset() || member.digest.is_zero())) {
      return Status::INVALID;
    }
  }
  return Status::OK;
}

const ExpectedMember* SiteExpectation::find(const MemberDomainKey& domain) const noexcept {
  for (const auto& member : members) {
    if (member.domain == domain) {
      return &member;
    }
  }
  return nullptr;
}

std::size_t SiteExpectation::required_count() const noexcept {
  std::size_t count = 0;
  for (const auto& member : members) {
    if (member.required) {
      ++count;
    }
  }
  return count;
}

std::size_t SiteExpectation::pinned_count() const noexcept {
  std::size_t count = 0;
  for (const auto& member : members) {
    if (member.pinned) {
      ++count;
    }
  }
  return count;
}

// ---------------------------------------------------------------------------
// Records and composed pieces
// ---------------------------------------------------------------------------

std::string MemberDomainState::to_string() const {
  std::string out = domain.to_string();
  out += " ";
  out += site_fabric::to_string(lifecycle);
  out += " gen=" + generation.to_string();
  out += " digest=" + digest.short_hex();
  out += " evidence=";
  out += site_fabric::to_string(evidence_state);
  if (!factors.empty()) {
    out += " [";
    out += factors.join(",");
    out += "]";
  }
  return out;
}

std::string MemberPublicationRecord::to_string() const {
  return source().to_string() + " by=" + publisher.value() + " incarnation=" +
         incarnation.to_string() + " accepted=" + std::to_string(acceptance_sequence);
}

std::string MemberRetirementRecord::to_string() const {
  return domain.to_string() + "@" + generation.to_string() + " reason=" + reason;
}

std::string DecisionId::to_string() const {
  std::string out = site_fabric::to_string(kind);
  if (!subject.empty()) {
    out += "/" + subject.to_string();
  }
  return out;
}

std::string SiteDecision::to_string() const {
  std::string out = id.to_string();
  out += " ";
  out += site_fabric::to_string(status);
  out += " sources=" + std::to_string(sources.size());
  if (!factors.empty()) {
    out += " [";
    out += factors.join(",");
    out += "]";
  }
  return out;
}

std::string ResourceAttribution::to_string() const {
  std::string out = key.to_string();
  out += " ";
  out += site_fabric::to_string(status);
  out += " attestors=" + std::to_string(attestors.size());
  out += " contents=" + std::to_string(distinct_contents);
  return out;
}

std::string CapacityLedgerEntry::to_string() const {
  std::string out = owner.to_string();
  out += "/";
  out += site_fabric::to_string(scope);
  out += " ";
  out += site_fabric::to_string(status);
  out += " ";
  out += capacity.to_string();
  out += counted_in_total ? " counted" : " excluded";
  return out;
}

const CapacityLedgerEntry* CapacityLedger::find(const ResourceKey& owner) const noexcept {
  for (const auto& entry : entries) {
    if (entry.owner == owner) {
      return &entry;
    }
  }
  return nullptr;
}

std::string CapacityLedger::to_string() const {
  std::string out = "entries=" + std::to_string(entries.size());
  out += " total[" + total.to_string() + "]";
  out += " available[" + available.to_string() + "]";
  out += " excluded[" + excluded_by_maintenance.to_string() + "]";
  out += " indeterminate[" + indeterminate.to_string() + "]";
  out += closes_exactly ? " closes" : " does-not-close";
  out += " ";
  out += site_fabric::to_string(status);
  return out;
}

std::string ConnectivitySummary::to_string() const {
  std::string out = "links=" + std::to_string(shared_links);
  out += "(up=" + std::to_string(shared_links_up);
  out += ",down=" + std::to_string(shared_links_down);
  out += ",degraded=" + std::to_string(shared_links_degraded);
  out += ",unknown=" + std::to_string(shared_links_unknown) + ")";
  out += " gateways=" + std::to_string(gateways);
  out += "(up=" + std::to_string(gateways_up);
  out += ",down=" + std::to_string(gateways_down);
  out += ",degraded=" + std::to_string(gateways_degraded);
  out += ",unknown=" + std::to_string(gateways_unknown) + ")";
  out += " ";
  out += site_fabric::to_string(status);
  return out;
}

std::string SiteConflict::to_string() const {
  std::string out = site_fabric::to_string(code);
  if (!subject.empty()) {
    out += " " + subject.to_string();
  }
  out += " parties=" + std::to_string(parties.size());
  if (!variants.empty()) {
    out += " variants=" + std::to_string(variants.size());
  }
  return out;
}

std::string SiteDeficit::to_string() const {
  std::string out = site_fabric::to_string(code);
  out += " ";
  out += domain.to_string();
  out += ": ";
  out += detail;
  return out;
}

std::string MaintenanceEffect::to_string() const {
  std::string out = id.value();
  out += " ";
  out += site_fabric::to_string(state);
  out += " excluded=" + std::to_string(excluded.size());
  out += " indeterminate=" + std::to_string(indeterminate.size());
  out += " ";
  out += site_fabric::to_string(status);
  return out;
}

std::string ObligationVerdict::to_string() const {
  std::string out = id.value();
  out += " ";
  out += site_fabric::to_string(kind);
  out += " ";
  out += site_fabric::to_string(status);
  out += " required=" + std::to_string(required);
  out += observed_known ? " observed=" + std::to_string(observed) : " observed=UNKNOWN";
  return out;
}

Digest ComposedSite::compute_digest() const {
  return internal::composed_site_digest(*this);
}

std::string ComposedSite::to_string() const {
  std::string out = site.value();
  out += " epoch=" + epoch.to_string();
  out += " gen=" + generation.to_string();
  out += " ";
  out += site_fabric::to_string(lifecycle);
  out += " members=" + std::to_string(members.size());
  out += " conflicts=" + std::to_string(conflicts.size());
  out += " deficits=" + std::to_string(deficits.size());
  out += " " + capacity.to_string();
  return out;
}

const MemberDomainState* ComposedSite::find_member(const MemberDomainKey& domain) const {
  for (const auto& member : members) {
    if (member.domain == domain) {
      return &member;
    }
  }
  return nullptr;
}

const ResourceAttribution* ComposedSite::find_ownership(const ResourceKey& key) const {
  for (const auto& attribution : ownership) {
    if (attribution.key == key) {
      return &attribution;
    }
  }
  return nullptr;
}

const SiteDecision* ComposedSite::find_decision(const DecisionId& id) const {
  for (const auto& decision : decisions) {
    if (decision.id == id) {
      return &decision;
    }
  }
  return nullptr;
}

std::size_t ComposedSite::count_decisions(DecisionKind kind) const {
  std::size_t count = 0;
  for (const auto& decision : decisions) {
    if (decision.id.kind == kind) {
      ++count;
    }
  }
  return count;
}

}  // namespace site_fabric
