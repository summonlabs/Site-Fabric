// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Full codecs.
//
// The readers here are the only place untrusted bytes become model values, so
// every one of them follows the same discipline: read the length, compare it
// with the bound, refuse before allocating, and return a distinguishing status.
// A short buffer is TRUNCATED, an out-of-range length is LIMIT_EXCEEDED, an
// unknown enumeration value is MALFORMED, and trailing bytes are MALFORMED
// too, because a schema that no longer matches must not decode silently.

#include <cstddef>
#include <string>
#include <vector>

#include "core/digest.hpp"
#include "site_fabric/limits.hpp"

namespace site_fabric::internal {

// --- Primitive readers -----------------------------------------------------

[[nodiscard]] Status read_identifier(CanonicalReader& reader, std::string& out) {
  const Status status = reader.text(out, limits::kMaxIdentifierBytes);
  if (!is_ok(status)) {
    return status;
  }
  return is_valid_identifier(out) ? Status::OK : Status::MALFORMED;
}

/// Reads an identity that may legitimately be absent. An expected member that
/// never reported has no publisher, and saying so must not be a decode error.
[[nodiscard]] Status read_optional_identifier(CanonicalReader& reader, std::string& out) {
  const Status status = reader.text(out, limits::kMaxIdentifierBytes);
  if (!is_ok(status)) {
    return status;
  }
  if (out.empty()) {
    return Status::OK;
  }
  return is_valid_identifier(out) ? Status::OK : Status::MALFORMED;
}

template <class Enum>
[[nodiscard]] Status read_enum(CanonicalReader& reader, std::uint8_t limit, Enum& out) {
  std::uint8_t value = 0;
  const Status status = reader.u8(value);
  if (!is_ok(status)) {
    return status;
  }
  if (value > limit) {
    return Status::MALFORMED;
  }
  out = static_cast<Enum>(value);
  return Status::OK;
}

[[nodiscard]] Status read_member_domain_key(CanonicalReader& reader, MemberDomainKey& out) {
  std::uint8_t kind = 0;
  const Status first = reader.u8(kind);
  if (!is_ok(first)) {
    return first;
  }
  std::string id;
  if (kind == static_cast<std::uint8_t>(MemberDomainKind::UNKNOWN)) {
    // An absent domain is a real value: a deficit that belongs to the site
    // rather than to a member domain carries no key at all.
    const Status empty = reader.text(id, limits::kMaxIdentifierBytes);
    if (!is_ok(empty)) {
      return empty;
    }
    if (!id.empty()) {
      return Status::MALFORMED;
    }
    out = MemberDomainKey{};
    return Status::OK;
  }
  if (!is_valid(static_cast<MemberDomainKind>(kind))) {
    return Status::MALFORMED;
  }
  const Status second = read_identifier(reader, id);
  if (!is_ok(second)) {
    return second;
  }
  out.kind = static_cast<MemberDomainKind>(kind);
  out.id = std::move(id);
  return Status::OK;
}

[[nodiscard]] Status read_resource_key(CanonicalReader& reader, ResourceKey& out) {
  std::uint8_t kind = 0;
  const Status first = reader.u8(kind);
  if (!is_ok(first)) {
    return first;
  }
  std::string id;
  if (kind == static_cast<std::uint8_t>(ResourceKind::UNKNOWN)) {
    // A site-wide decision has no subject. That is a value, not a defect, and
    // it round-trips as an empty key rather than being refused.
    const Status empty = reader.text(id, limits::kMaxIdentifierBytes);
    if (!is_ok(empty)) {
      return empty;
    }
    if (!id.empty()) {
      return Status::MALFORMED;
    }
    out = ResourceKey{};
    return Status::OK;
  }
  if (!is_valid(static_cast<ResourceKind>(kind))) {
    return Status::MALFORMED;
  }
  const Status second = read_identifier(reader, id);
  if (!is_ok(second)) {
    return second;
  }
  out.kind = static_cast<ResourceKind>(kind);
  out.id = std::move(id);
  return Status::OK;
}

[[nodiscard]] Status read_capacity_value(CanonicalReader& reader, CapacityValue& out) {
  bool known = false;
  const Status first = reader.flag(known);
  if (!is_ok(first)) {
    return first;
  }
  std::uint64_t value = 0;
  const Status second = reader.u64(value);
  if (!is_ok(second)) {
    return second;
  }
  if (known && value > limits::kMaxCapacityBps) {
    return Status::LIMIT_EXCEEDED;
  }
  if (!known && value != 0) {
    return Status::MALFORMED;
  }
  out.known = known;
  out.bps = value;
  return Status::OK;
}

[[nodiscard]] Status read_capacity(CanonicalReader& reader, CapacityVector& out) {
  Status status = read_capacity_value(reader, out.ingress);
  if (!is_ok(status)) {
    return status;
  }
  status = read_capacity_value(reader, out.egress);
  if (!is_ok(status)) {
    return status;
  }
  return read_capacity_value(reader, out.internal);
}

[[nodiscard]] Status read_evidence(CanonicalReader& reader, Evidence& out) {
  Status status = read_enum(reader, static_cast<std::uint8_t>(Provenance::RECONSTRUCTED),
                            out.provenance);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.i64(out.observed_at_ms);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.u64(out.ttl_ms);
  if (!is_ok(status)) {
    return status;
  }
  if (out.ttl_ms > limits::kMaxEvidenceTtlMs) {
    return Status::LIMIT_EXCEEDED;
  }
  return reader.digest(out.payload_digest);
}

[[nodiscard]] Status read_factors(CanonicalReader& reader, Factors& out) {
  std::size_t count = 0;
  const Status status = reader.count(count, limits::kMaxFactors + 1);
  if (!is_ok(status)) {
    return status;
  }
  for (std::size_t index = 0; index < count; ++index) {
    std::string factor;
    const Status read = reader.text(factor, limits::kMaxTextBytes);
    if (!is_ok(read)) {
      return read;
    }
    out.add(std::move(factor));
  }
  return Status::OK;
}

[[nodiscard]] Status read_source_set(CanonicalReader& reader, SourceSet& out) {
  std::size_t count = 0;
  const Status status = reader.count(count, limits::kMaxDecisionSources + 1);
  if (!is_ok(status)) {
    return status;
  }
  for (std::size_t index = 0; index < count; ++index) {
    SourceRef ref;
    const Status read = reader.source_ref(ref);
    if (!is_ok(read)) {
      return read;
    }
    out.insert(ref);
  }
  return Status::OK;
}

namespace {

// --- Claim readers ---------------------------------------------------------

[[nodiscard]] Status read_rack_claim(CanonicalReader& reader, RackClaim& out) {
  Status status = read_strong_id(reader, out.id);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.generation(out.generation);
  if (!is_ok(status)) {
    return status;
  }
  status = read_capacity(reader, out.local_capacity);
  if (!is_ok(status)) {
    return status;
  }
  status = read_enum(reader, static_cast<std::uint8_t>(ConnectivityState::DEGRADED), out.uplink);
  if (!is_ok(status)) {
    return status;
  }
  return read_evidence(reader, out.evidence);
}

[[nodiscard]] Status read_pod_claim(CanonicalReader& reader, PodClaim& out) {
  Status status = read_strong_id(reader, out.id);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.generation(out.generation);
  if (!is_ok(status)) {
    return status;
  }
  std::size_t count = 0;
  status = reader.count(count, limits::kMaxResourceMembers + 1);
  if (!is_ok(status)) {
    return status;
  }
  out.racks.resize(count);
  for (std::size_t index = 0; index < count; ++index) {
    status = read_strong_id(reader, out.racks[index]);
    if (!is_ok(status)) {
      return status;
    }
  }
  return read_evidence(reader, out.evidence);
}

[[nodiscard]] Status read_cluster_claim(CanonicalReader& reader, ClusterClaim& out) {
  Status status = read_strong_id(reader, out.id);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.generation(out.generation);
  if (!is_ok(status)) {
    return status;
  }
  std::size_t count = 0;
  status = reader.count(count, limits::kMaxResourceMembers + 1);
  if (!is_ok(status)) {
    return status;
  }
  out.pods.resize(count);
  for (std::size_t index = 0; index < count; ++index) {
    status = read_strong_id(reader, out.pods[index]);
    if (!is_ok(status)) {
      return status;
    }
  }
  status = reader.count(count, limits::kMaxResourceMembers + 1);
  if (!is_ok(status)) {
    return status;
  }
  out.racks.resize(count);
  for (std::size_t index = 0; index < count; ++index) {
    status = read_strong_id(reader, out.racks[index]);
    if (!is_ok(status)) {
      return status;
    }
  }
  return read_evidence(reader, out.evidence);
}

[[nodiscard]] Status read_shared_link_claim(CanonicalReader& reader, SharedLinkClaim& out) {
  Status status = read_strong_id(reader, out.id);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.flag(out.external);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.generation(out.generation);
  if (!is_ok(status)) {
    return status;
  }
  status = read_capacity(reader, out.capacity);
  if (!is_ok(status)) {
    return status;
  }
  status = read_enum(reader, static_cast<std::uint8_t>(ConnectivityState::DEGRADED), out.state);
  if (!is_ok(status)) {
    return status;
  }
  return read_evidence(reader, out.evidence);
}

[[nodiscard]] Status read_gateway_claim(CanonicalReader& reader, GatewayClaim& out) {
  Status status = read_strong_id(reader, out.id);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.generation(out.generation);
  if (!is_ok(status)) {
    return status;
  }
  status = read_capacity(reader, out.capacity);
  if (!is_ok(status)) {
    return status;
  }
  status = read_enum(reader, static_cast<std::uint8_t>(ConnectivityState::DEGRADED), out.state);
  if (!is_ok(status)) {
    return status;
  }
  return read_evidence(reader, out.evidence);
}

[[nodiscard]] Status read_capacity_contribution(CanonicalReader& reader,
                                                CapacityContribution& out) {
  Status status = read_resource_key(reader, out.owner);
  if (!is_ok(status)) {
    return status;
  }
  status = read_enum(reader, static_cast<std::uint8_t>(CapacityScope::GATEWAY), out.scope);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.generation(out.generation);
  if (!is_ok(status)) {
    return status;
  }
  status = read_capacity(reader, out.capacity);
  if (!is_ok(status)) {
    return status;
  }
  return read_evidence(reader, out.evidence);
}

[[nodiscard]] Status read_failure_domain_claim(CanonicalReader& reader, FailureDomainClaim& out) {
  Status status = read_strong_id(reader, out.id);
  if (!is_ok(status)) {
    return status;
  }
  FailureDomainClass domain_class = FailureDomainClass::UNKNOWN;
  status = read_enum(reader, static_cast<std::uint8_t>(FailureDomainClass::ADMINISTRATIVE),
                     domain_class);
  if (!is_ok(status)) {
    return status;
  }
  out.domain_class = domain_class;

  bool present = false;
  status = reader.optional(present);
  if (!is_ok(status)) {
    return status;
  }
  if (present) {
    FailureDomainId parent;
    status = read_strong_id(reader, parent);
    if (!is_ok(status)) {
      return status;
    }
    out.parent = parent;
  } else {
    out.parent.reset();
  }

  std::size_t count = 0;
  status = reader.count(count, limits::kMaxResourceMembers + 1);
  if (!is_ok(status)) {
    return status;
  }
  out.members.resize(count);
  for (std::size_t index = 0; index < count; ++index) {
    status = read_resource_key(reader, out.members[index]);
    if (!is_ok(status)) {
      return status;
    }
  }
  status = reader.generation(out.generation);
  if (!is_ok(status)) {
    return status;
  }
  return read_evidence(reader, out.evidence);
}

[[nodiscard]] Status read_maintenance_zone(CanonicalReader& reader, MaintenanceZone& out) {
  Status status = read_strong_id(reader, out.id);
  if (!is_ok(status)) {
    return status;
  }
  status = read_enum(reader, static_cast<std::uint8_t>(MaintenanceState::CANCELLED), out.state);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.i64(out.window_start_ms);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.i64(out.window_end_ms);
  if (!is_ok(status)) {
    return status;
  }
  std::size_t count = 0;
  status = reader.count(count, limits::kMaxFailureDomainsPerDeclaration + 1);
  if (!is_ok(status)) {
    return status;
  }
  out.domains.resize(count);
  for (std::size_t index = 0; index < count; ++index) {
    status = read_strong_id(reader, out.domains[index]);
    if (!is_ok(status)) {
      return status;
    }
  }
  status = reader.count(count, limits::kMaxResourceMembers + 1);
  if (!is_ok(status)) {
    return status;
  }
  out.resources.resize(count);
  for (std::size_t index = 0; index < count; ++index) {
    status = read_resource_key(reader, out.resources[index]);
    if (!is_ok(status)) {
      return status;
    }
  }
  status = reader.generation(out.generation);
  if (!is_ok(status)) {
    return status;
  }
  return read_evidence(reader, out.evidence);
}

[[nodiscard]] Status read_protected_obligation(CanonicalReader& reader,
                                               ProtectedObligation& out) {
  Status status = read_strong_id(reader, out.id);
  if (!is_ok(status)) {
    return status;
  }
  status = read_enum(reader, static_cast<std::uint8_t>(ObligationKind::DOMAIN_SURVIVABILITY),
                     out.kind);
  if (!is_ok(status)) {
    return status;
  }
  bool present = false;
  status = reader.optional(present);
  if (!is_ok(status)) {
    return status;
  }
  if (present) {
    FailureDomainId domain;
    status = read_strong_id(reader, domain);
    if (!is_ok(status)) {
      return status;
    }
    out.scope_domain = domain;
  } else {
    out.scope_domain.reset();
  }
  status = reader.optional(present);
  if (!is_ok(status)) {
    return status;
  }
  if (present) {
    ResourceKey resource;
    status = read_resource_key(reader, resource);
    if (!is_ok(status)) {
      return status;
    }
    out.scope_resource = resource;
  } else {
    out.scope_resource.reset();
  }
  status = reader.u64(out.required);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.flag(out.protected_from_maintenance);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.generation(out.generation);
  if (!is_ok(status)) {
    return status;
  }
  return read_evidence(reader, out.evidence);
}

}  // namespace

// ---------------------------------------------------------------------------
// Declaration
// ---------------------------------------------------------------------------

void write_declaration(CanonicalWriter& writer, const MemberDomainDeclaration& declaration) {
  write_declaration_content(writer, declaration);
}

Status read_declaration(CanonicalReader& reader, MemberDomainDeclaration& out) {
  Status status = read_member_domain_key(reader, out.domain);
  if (!is_ok(status)) {
    return status;
  }
  status = read_strong_id(reader, out.site);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.generation(out.generation);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.schema(out.schema);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.epoch(out.observed_epoch);
  if (!is_ok(status)) {
    return status;
  }
  status = read_evidence(reader, out.evidence);
  if (!is_ok(status)) {
    return status;
  }

  std::size_t count = 0;

  status = reader.count(count, limits::kMaxClaimLists + 1);
  if (!is_ok(status)) {
    return status;
  }
  out.clusters.resize(count);
  for (auto& claim : out.clusters) {
    status = read_cluster_claim(reader, claim);
    if (!is_ok(status)) {
      return status;
    }
  }

  status = reader.count(count, limits::kMaxClaimLists + 1);
  if (!is_ok(status)) {
    return status;
  }
  out.pods.resize(count);
  for (auto& claim : out.pods) {
    status = read_pod_claim(reader, claim);
    if (!is_ok(status)) {
      return status;
    }
  }

  status = reader.count(count, limits::kMaxClaimLists + 1);
  if (!is_ok(status)) {
    return status;
  }
  out.racks.resize(count);
  for (auto& claim : out.racks) {
    status = read_rack_claim(reader, claim);
    if (!is_ok(status)) {
      return status;
    }
  }

  status = reader.count(count, limits::kMaxClaimLists + 1);
  if (!is_ok(status)) {
    return status;
  }
  out.shared_links.resize(count);
  for (auto& claim : out.shared_links) {
    status = read_shared_link_claim(reader, claim);
    if (!is_ok(status)) {
      return status;
    }
  }

  status = reader.count(count, limits::kMaxClaimLists + 1);
  if (!is_ok(status)) {
    return status;
  }
  out.gateways.resize(count);
  for (auto& claim : out.gateways) {
    status = read_gateway_claim(reader, claim);
    if (!is_ok(status)) {
      return status;
    }
  }

  status = reader.count(count, limits::kMaxCapacityContributions + 1);
  if (!is_ok(status)) {
    return status;
  }
  out.capacity.resize(count);
  for (auto& claim : out.capacity) {
    status = read_capacity_contribution(reader, claim);
    if (!is_ok(status)) {
      return status;
    }
  }

  status = reader.count(count, limits::kMaxFailureDomainsPerDeclaration + 1);
  if (!is_ok(status)) {
    return status;
  }
  out.failure_domains.resize(count);
  for (auto& claim : out.failure_domains) {
    status = read_failure_domain_claim(reader, claim);
    if (!is_ok(status)) {
      return status;
    }
  }

  status = reader.count(count, limits::kMaxMaintenanceZonesPerDeclaration + 1);
  if (!is_ok(status)) {
    return status;
  }
  out.maintenance_zones.resize(count);
  for (auto& claim : out.maintenance_zones) {
    status = read_maintenance_zone(reader, claim);
    if (!is_ok(status)) {
      return status;
    }
  }

  status = reader.count(count, limits::kMaxObligationsPerDeclaration + 1);
  if (!is_ok(status)) {
    return status;
  }
  out.obligations.resize(count);
  for (auto& claim : out.obligations) {
    status = read_protected_obligation(reader, claim);
    if (!is_ok(status)) {
      return status;
    }
  }

  // The digest is not carried on the wire: it is derived from the content, so
  // recomputing it here is both the round trip and the integrity check.
  out.digest = out.compute_digest();
  return Status::OK;
}

// ---------------------------------------------------------------------------
// Expectation
// ---------------------------------------------------------------------------

void write_expectation(CanonicalWriter& writer, const SiteExpectation& expectation) {
  writer.text(expectation.site.value());
  writer.schema(expectation.schema);
  writer.epoch(expectation.minimum_epoch);
  writer.flag(expectation.reject_unexpected);
  writer.count(expectation.members.size());
  for (const auto& member : expectation.members) {
    write_member_domain_key(writer, member.domain);
    writer.flag(member.pinned);
    writer.generation(member.generation);
    writer.digest(member.digest);
    writer.flag(member.required);
  }
}

Status read_expectation(CanonicalReader& reader, SiteExpectation& out) {
  Status status = read_strong_id(reader, out.site);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.schema(out.schema);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.epoch(out.minimum_epoch);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.flag(out.reject_unexpected);
  if (!is_ok(status)) {
    return status;
  }
  std::size_t count = 0;
  status = reader.count(count, limits::kMaxExpectedMembers + 1);
  if (!is_ok(status)) {
    return status;
  }
  out.members.resize(count);
  for (auto& member : out.members) {
    status = read_member_domain_key(reader, member.domain);
    if (!is_ok(status)) {
      return status;
    }
    status = reader.flag(member.pinned);
    if (!is_ok(status)) {
      return status;
    }
    status = reader.generation(member.generation);
    if (!is_ok(status)) {
      return status;
    }
    status = reader.digest(member.digest);
    if (!is_ok(status)) {
      return status;
    }
    status = reader.flag(member.required);
    if (!is_ok(status)) {
      return status;
    }
  }
  return Status::OK;
}

// ---------------------------------------------------------------------------
// Records
// ---------------------------------------------------------------------------

void write_publication_record(CanonicalWriter& writer, const MemberPublicationRecord& record) {
  write_member_domain_key(writer, record.domain);
  writer.generation(record.generation);
  writer.digest(record.digest);
  writer.schema(record.schema);
  writer.text(record.site.value());
  writer.epoch(record.observed_epoch);
  writer.incarnation(record.incarnation);
  writer.text(record.publisher.value());
  writer.u64(record.attempt.sequence);
  writer.u32(record.attempt.attempt);
  writer.i64(record.received_at_ms);
  writer.u64(record.acceptance_sequence);
  writer.flag(record.has_declaration);
  if (record.has_declaration) {
    write_declaration(writer, record.declaration);
  }
}

Status read_publication_record(CanonicalReader& reader, MemberPublicationRecord& out) {
  Status status = read_member_domain_key(reader, out.domain);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.generation(out.generation);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.digest(out.digest);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.schema(out.schema);
  if (!is_ok(status)) {
    return status;
  }
  status = read_strong_id(reader, out.site);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.epoch(out.observed_epoch);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.incarnation(out.incarnation);
  if (!is_ok(status)) {
    return status;
  }
  status = read_strong_id(reader, out.publisher);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.u64(out.attempt.sequence);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.u32(out.attempt.attempt);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.i64(out.received_at_ms);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.u64(out.acceptance_sequence);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.flag(out.has_declaration);
  if (!is_ok(status)) {
    return status;
  }
  if (!out.has_declaration) {
    out.declaration = MemberDomainDeclaration{};
    return Status::OK;
  }
  status = read_declaration(reader, out.declaration);
  if (!is_ok(status)) {
    return status;
  }
  out.declaration.digest = out.digest;
  return Status::OK;
}

void write_retirement_record(CanonicalWriter& writer, const MemberRetirementRecord& record) {
  write_member_domain_key(writer, record.domain);
  writer.generation(record.generation);
  writer.incarnation(record.incarnation);
  writer.i64(record.received_at_ms);
  writer.u64(record.acceptance_sequence);
  writer.text(record.reason);
}

Status read_retirement_record(CanonicalReader& reader, MemberRetirementRecord& out) {
  Status status = read_member_domain_key(reader, out.domain);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.generation(out.generation);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.incarnation(out.incarnation);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.i64(out.received_at_ms);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.u64(out.acceptance_sequence);
  if (!is_ok(status)) {
    return status;
  }
  return reader.text(out.reason, limits::kMaxTextBytes);
}

void write_authority_token(CanonicalWriter& writer, const AuthorityToken& token) {
  writer.u8(static_cast<std::uint8_t>(token.kind));
  writer.text(token.holder);
  writer.text(token.site.value());
  writer.epoch(token.epoch);
  writer.incarnation(token.incarnation);
  writer.generation(token.generation);
  writer.u64(token.lease_id);
  writer.i64(token.issued_at_ms);
  writer.i64(token.expires_at_ms);
}

Status read_authority_token(CanonicalReader& reader, AuthorityToken& out) {
  Status status = read_enum(reader, static_cast<std::uint8_t>(AuthorityKind::OBSERVER), out.kind);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.text(out.holder, limits::kMaxIdentifierBytes);
  if (!is_ok(status)) {
    return status;
  }
  status = read_strong_id(reader, out.site);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.epoch(out.epoch);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.incarnation(out.incarnation);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.generation(out.generation);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.u64(out.lease_id);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.i64(out.issued_at_ms);
  if (!is_ok(status)) {
    return status;
  }
  return reader.i64(out.expires_at_ms);
}


namespace {

// --- Composed-site writers -------------------------------------------------

void write_member_state(CanonicalWriter& writer, const MemberDomainState& state) {
  write_member_domain_key(writer, state.domain);
  writer.u8(static_cast<std::uint8_t>(state.lifecycle));
  writer.u8(static_cast<std::uint8_t>(state.status));
  writer.generation(state.generation);
  writer.digest(state.digest);
  writer.schema(state.schema);
  writer.incarnation(state.incarnation);
  writer.text(state.publisher.value());
  writer.i64(state.received_at_ms);
  writer.u8(static_cast<std::uint8_t>(state.evidence_state));
  writer.flag(state.expected);
  writer.flag(state.expectation_pinned);
  writer.source_ref(state.source);
  write_factors(writer, state.factors);
}

void write_ownership(CanonicalWriter& writer, const ResourceAttribution& attribution) {
  write_resource_key(writer, attribution.key);
  write_source_set(writer, attribution.attestors);
  writer.digest(attribution.consensus_digest);
  writer.u64(attribution.distinct_contents);
  writer.u8(static_cast<std::uint8_t>(attribution.status));
  write_factors(writer, attribution.factors);
}

void write_ledger_entry(CanonicalWriter& writer, const CapacityLedgerEntry& entry) {
  write_resource_key(writer, entry.owner);
  writer.u8(static_cast<std::uint8_t>(entry.scope));
  write_capacity(writer, entry.capacity);
  write_source_set(writer, entry.attestors);
  writer.u8(static_cast<std::uint8_t>(entry.status));
  writer.flag(entry.counted_in_total);
  write_factors(writer, entry.factors);
}

void write_forest_node(CanonicalWriter& writer, const FailureDomainNode& node) {
  writer.text(node.id.value());
  writer.u8(static_cast<std::uint8_t>(node.domain_class));
  writer.optional(node.parent.has_value());
  if (node.parent.has_value()) {
    writer.text(node.parent->value());
  }
  writer.count(node.members.size());
  for (const auto& member : node.members) {
    write_resource_key(writer, member);
  }
  writer.count(node.children.size());
  for (const auto& child : node.children) {
    writer.text(child.value());
  }
  writer.count(node.ancestors.size());
  for (const auto& ancestor : node.ancestors) {
    writer.text(ancestor.value());
  }
  writer.u32(node.depth);
  write_source_set(writer, node.attestors);
  writer.u8(static_cast<std::uint8_t>(node.status));
}

void write_maintenance_effect(CanonicalWriter& writer, const MaintenanceEffect& effect) {
  writer.text(effect.id.value());
  writer.u8(static_cast<std::uint8_t>(effect.state));
  writer.count(effect.excluded.size());
  for (const auto& resource : effect.excluded) {
    write_resource_key(writer, resource);
  }
  writer.count(effect.indeterminate.size());
  for (const auto& resource : effect.indeterminate) {
    write_resource_key(writer, resource);
  }
  writer.count(effect.overlapping_obligations.size());
  for (const auto& obligation : effect.overlapping_obligations) {
    writer.text(obligation.value());
  }
  write_capacity(writer, effect.excluded_capacity);
  writer.u8(static_cast<std::uint8_t>(effect.status));
  write_factors(writer, effect.factors);
}

void write_verdict(CanonicalWriter& writer, const ObligationVerdict& verdict) {
  writer.text(verdict.id.value());
  writer.u8(static_cast<std::uint8_t>(verdict.kind));
  writer.u8(static_cast<std::uint8_t>(verdict.status));
  writer.u64(verdict.required);
  writer.u64(verdict.observed);
  writer.flag(verdict.observed_known);
  write_source_set(writer, verdict.sources);
  write_factors(writer, verdict.factors);
}

void write_conflict(CanonicalWriter& writer, const SiteConflict& conflict) {
  writer.u8(static_cast<std::uint8_t>(conflict.code));
  write_resource_key(writer, conflict.subject);
  write_source_set(writer, conflict.parties);
  writer.count(conflict.variants.size());
  for (const auto& variant : conflict.variants) {
    writer.text(variant);
  }
  writer.i64(conflict.detected_at_ms);
}

void write_deficit(CanonicalWriter& writer, const SiteDeficit& deficit) {
  writer.u8(static_cast<std::uint8_t>(deficit.code));
  write_member_domain_key(writer, deficit.domain);
  writer.text(deficit.detail);
}

void write_decision(CanonicalWriter& writer, const SiteDecision& decision) {
  writer.u8(static_cast<std::uint8_t>(decision.id.kind));
  write_resource_key(writer, decision.id.subject);
  writer.u8(static_cast<std::uint8_t>(decision.status));
  write_source_set(writer, decision.sources);
  writer.digest(decision.value_digest);
  write_factors(writer, decision.factors);
}

void write_connectivity(CanonicalWriter& writer, const ConnectivitySummary& summary) {
  writer.u64(summary.shared_links);
  writer.u64(summary.shared_links_up);
  writer.u64(summary.shared_links_down);
  writer.u64(summary.shared_links_degraded);
  writer.u64(summary.shared_links_unknown);
  writer.u64(summary.external_links);
  writer.u64(summary.internal_links);
  writer.u64(summary.gateways);
  writer.u64(summary.gateways_up);
  writer.u64(summary.gateways_down);
  writer.u64(summary.gateways_degraded);
  writer.u64(summary.gateways_unknown);
  writer.count(summary.unknown_state.size());
  for (const auto& resource : summary.unknown_state) {
    write_resource_key(writer, resource);
  }
  write_source_set(writer, summary.sources);
  writer.u8(static_cast<std::uint8_t>(summary.status));
}

// --- Composed-site readers -------------------------------------------------

Status read_member_state(CanonicalReader& reader, MemberDomainState& out) {
  Status status = read_member_domain_key(reader, out.domain);
  if (!is_ok(status)) {
    return status;
  }
  status = read_enum(reader, static_cast<std::uint8_t>(MemberLifecycle::REVOKED), out.lifecycle);
  if (!is_ok(status)) {
    return status;
  }
  status = read_enum(reader, static_cast<std::uint8_t>(Status::NO_AUTHORITATIVE_SOURCE), out.status);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.generation(out.generation);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.digest(out.digest);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.schema(out.schema);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.incarnation(out.incarnation);
  if (!is_ok(status)) {
    return status;
  }
  std::string publisher;
  status = read_optional_identifier(reader, publisher);
  if (!is_ok(status)) {
    return status;
  }
  out.publisher = publisher.empty() ? MemberId{} : MemberId::unchecked(std::move(publisher));
  status = reader.i64(out.received_at_ms);
  if (!is_ok(status)) {
    return status;
  }
  status = read_enum(reader,
                     static_cast<std::uint8_t>(EvidenceState::RECONSTRUCTED_NEEDS_REVALIDATION),
                     out.evidence_state);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.flag(out.expected);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.flag(out.expectation_pinned);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.source_ref(out.source);
  if (!is_ok(status)) {
    return status;
  }
  return read_factors(reader, out.factors);
}

Status read_ownership(CanonicalReader& reader, ResourceAttribution& out) {
  Status status = read_resource_key(reader, out.key);
  if (!is_ok(status)) {
    return status;
  }
  status = read_source_set(reader, out.attestors);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.digest(out.consensus_digest);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.u64(out.distinct_contents);
  if (!is_ok(status)) {
    return status;
  }
  status = read_enum(reader, static_cast<std::uint8_t>(Status::NO_AUTHORITATIVE_SOURCE), out.status);
  if (!is_ok(status)) {
    return status;
  }
  return read_factors(reader, out.factors);
}

Status read_ledger_entry(CanonicalReader& reader, CapacityLedgerEntry& out) {
  Status status = read_resource_key(reader, out.owner);
  if (!is_ok(status)) {
    return status;
  }
  status = read_enum(reader, static_cast<std::uint8_t>(CapacityScope::GATEWAY), out.scope);
  if (!is_ok(status)) {
    return status;
  }
  status = read_capacity(reader, out.capacity);
  if (!is_ok(status)) {
    return status;
  }
  status = read_source_set(reader, out.attestors);
  if (!is_ok(status)) {
    return status;
  }
  status = read_enum(reader, static_cast<std::uint8_t>(Status::NO_AUTHORITATIVE_SOURCE), out.status);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.flag(out.counted_in_total);
  if (!is_ok(status)) {
    return status;
  }
  return read_factors(reader, out.factors);
}

Status read_forest_node(CanonicalReader& reader, FailureDomainNode& out) {
  std::string id;
  Status status = read_identifier(reader, id);
  if (!is_ok(status)) {
    return status;
  }
  out.id = FailureDomainId::unchecked(std::move(id));

  status = read_enum(reader, static_cast<std::uint8_t>(FailureDomainClass::ADMINISTRATIVE),
                     out.domain_class);
  if (!is_ok(status)) {
    return status;
  }
  bool present = false;
  status = reader.optional(present);
  if (!is_ok(status)) {
    return status;
  }
  out.parent.reset();
  if (present) {
    FailureDomainId parent;
    status = read_strong_id(reader, parent);
    if (!is_ok(status)) {
      return status;
    }
    out.parent = parent;
  }
  std::size_t count = 0;
  status = reader.count(count, limits::kMaxResourceMembers + 1);
  if (!is_ok(status)) {
    return status;
  }
  out.members.resize(count);
  for (auto& member : out.members) {
    status = read_resource_key(reader, member);
    if (!is_ok(status)) {
      return status;
    }
  }
  status = reader.count(count, limits::kMaxDistinctFailureDomains + 1);
  if (!is_ok(status)) {
    return status;
  }
  out.children.resize(count);
  for (auto& child : out.children) {
    status = read_strong_id(reader, child);
    if (!is_ok(status)) {
      return status;
    }
  }
  status = reader.count(count, limits::kMaxFailureDomainDepth + 1);
  if (!is_ok(status)) {
    return status;
  }
  out.ancestors.resize(count);
  for (auto& ancestor : out.ancestors) {
    status = read_strong_id(reader, ancestor);
    if (!is_ok(status)) {
      return status;
    }
  }
  status = reader.u32(out.depth);
  if (!is_ok(status)) {
    return status;
  }
  if (out.depth > limits::kMaxFailureDomainDepth) {
    return Status::LIMIT_EXCEEDED;
  }
  status = read_source_set(reader, out.attestors);
  if (!is_ok(status)) {
    return status;
  }
  return read_enum(reader, static_cast<std::uint8_t>(Status::NO_AUTHORITATIVE_SOURCE), out.status);
}

Status read_maintenance_effect(CanonicalReader& reader, MaintenanceEffect& out) {
  std::string id;
  Status status = read_identifier(reader, id);
  if (!is_ok(status)) {
    return status;
  }
  out.id = MaintenanceZoneId::unchecked(std::move(id));
  status = read_enum(reader, static_cast<std::uint8_t>(MaintenanceState::CANCELLED), out.state);
  if (!is_ok(status)) {
    return status;
  }
  std::size_t count = 0;
  status = reader.count(count, limits::kMaxDistinctResources + 1);
  if (!is_ok(status)) {
    return status;
  }
  out.excluded.resize(count);
  for (auto& resource : out.excluded) {
    status = read_resource_key(reader, resource);
    if (!is_ok(status)) {
      return status;
    }
  }
  status = reader.count(count, limits::kMaxDistinctResources + 1);
  if (!is_ok(status)) {
    return status;
  }
  out.indeterminate.resize(count);
  for (auto& resource : out.indeterminate) {
    status = read_resource_key(reader, resource);
    if (!is_ok(status)) {
      return status;
    }
  }
  status = reader.count(count, limits::kMaxDistinctObligations + 1);
  if (!is_ok(status)) {
    return status;
  }
  out.overlapping_obligations.resize(count);
  for (auto& obligation : out.overlapping_obligations) {
    status = read_strong_id(reader, obligation);
    if (!is_ok(status)) {
      return status;
    }
  }
  status = read_capacity(reader, out.excluded_capacity);
  if (!is_ok(status)) {
    return status;
  }
  status = read_enum(reader, static_cast<std::uint8_t>(Status::NO_AUTHORITATIVE_SOURCE), out.status);
  if (!is_ok(status)) {
    return status;
  }
  return read_factors(reader, out.factors);
}

Status read_verdict(CanonicalReader& reader, ObligationVerdict& out) {
  std::string id;
  Status status = read_identifier(reader, id);
  if (!is_ok(status)) {
    return status;
  }
  out.id = ObligationId::unchecked(std::move(id));
  status = read_enum(reader, static_cast<std::uint8_t>(ObligationKind::DOMAIN_SURVIVABILITY),
                     out.kind);
  if (!is_ok(status)) {
    return status;
  }
  status = read_enum(reader, static_cast<std::uint8_t>(Status::NO_AUTHORITATIVE_SOURCE), out.status);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.u64(out.required);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.u64(out.observed);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.flag(out.observed_known);
  if (!is_ok(status)) {
    return status;
  }
  status = read_source_set(reader, out.sources);
  if (!is_ok(status)) {
    return status;
  }
  return read_factors(reader, out.factors);
}

Status read_conflict(CanonicalReader& reader, SiteConflict& out) {
  Status status =
      read_enum(reader, static_cast<std::uint8_t>(Status::NO_AUTHORITATIVE_SOURCE), out.code);
  if (!is_ok(status)) {
    return status;
  }
  status = read_resource_key(reader, out.subject);
  if (!is_ok(status)) {
    return status;
  }
  status = read_source_set(reader, out.parties);
  if (!is_ok(status)) {
    return status;
  }
  std::size_t count = 0;
  status = reader.count(count, limits::kMaxAttestorsPerResource + 1);
  if (!is_ok(status)) {
    return status;
  }
  out.variants.resize(count);
  for (auto& variant : out.variants) {
    status = reader.text(variant, limits::kMaxTextBytes);
    if (!is_ok(status)) {
      return status;
    }
  }
  return reader.i64(out.detected_at_ms);
}

Status read_deficit(CanonicalReader& reader, SiteDeficit& out) {
  Status status =
      read_enum(reader, static_cast<std::uint8_t>(Status::NO_AUTHORITATIVE_SOURCE), out.code);
  if (!is_ok(status)) {
    return status;
  }
  status = read_member_domain_key(reader, out.domain);
  if (!is_ok(status)) {
    return status;
  }
  return reader.text(out.detail, limits::kMaxTextBytes);
}

Status read_decision(CanonicalReader& reader, SiteDecision& out) {
  Status status =
      read_enum(reader, static_cast<std::uint8_t>(DecisionKind::SITE_LIFECYCLE), out.id.kind);
  if (!is_ok(status)) {
    return status;
  }
  status = read_resource_key(reader, out.id.subject);
  if (!is_ok(status)) {
    return status;
  }
  status = read_enum(reader, static_cast<std::uint8_t>(Status::NO_AUTHORITATIVE_SOURCE), out.status);
  if (!is_ok(status)) {
    return status;
  }
  status = read_source_set(reader, out.sources);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.digest(out.value_digest);
  if (!is_ok(status)) {
    return status;
  }
  return read_factors(reader, out.factors);
}

Status read_connectivity(CanonicalReader& reader, ConnectivitySummary& out) {
  Status status = reader.u64(out.shared_links);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.u64(out.shared_links_up);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.u64(out.shared_links_down);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.u64(out.shared_links_degraded);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.u64(out.shared_links_unknown);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.u64(out.external_links);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.u64(out.internal_links);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.u64(out.gateways);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.u64(out.gateways_up);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.u64(out.gateways_down);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.u64(out.gateways_degraded);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.u64(out.gateways_unknown);
  if (!is_ok(status)) {
    return status;
  }
  std::size_t count = 0;
  status = reader.count(count, limits::kMaxDistinctResources + 1);
  if (!is_ok(status)) {
    return status;
  }
  out.unknown_state.resize(count);
  for (auto& resource : out.unknown_state) {
    status = read_resource_key(reader, resource);
    if (!is_ok(status)) {
      return status;
    }
  }
  status = read_source_set(reader, out.sources);
  if (!is_ok(status)) {
    return status;
  }
  return read_enum(reader, static_cast<std::uint8_t>(Status::NO_AUTHORITATIVE_SOURCE), out.status);
}

}  // namespace

Digest decision_value_digest(const SiteDecision& decision) {
  CanonicalWriter writer;
  writer.u8(static_cast<std::uint8_t>(decision.id.kind));
  write_resource_key(writer, decision.id.subject);
  writer.u8(static_cast<std::uint8_t>(decision.status));
  write_factors(writer, decision.factors);
  return writer.finish_digest();
}

// ---------------------------------------------------------------------------
// Composed site
// ---------------------------------------------------------------------------

void write_composed_site(CanonicalWriter& writer, const ComposedSite& site) {
  writer.text(site.site.value());
  writer.epoch(site.epoch);
  writer.generation(site.generation);
  writer.incarnation(site.incarnation);
  writer.i64(site.composed_at_ms);
  writer.u8(static_cast<std::uint8_t>(site.lifecycle));
  writer.u8(static_cast<std::uint8_t>(site.status));
  writer.flag(site.membership_complete);
  writer.flag(site.all_evidence_fresh);
  writer.flag(site.any_conflict);
  writer.flag(site.any_unknown_capacity);

  writer.count(site.members.size());
  for (const auto& member : site.members) {
    write_member_state(writer, member);
  }
  writer.count(site.ownership.size());
  for (const auto& attribution : site.ownership) {
    write_ownership(writer, attribution);
  }
  writer.count(site.capacity.entries.size());
  for (const auto& entry : site.capacity.entries) {
    write_ledger_entry(writer, entry);
  }
  write_capacity(writer, site.capacity.total);
  write_capacity(writer, site.capacity.available);
  write_capacity(writer, site.capacity.excluded_by_maintenance);
  write_capacity(writer, site.capacity.indeterminate);
  writer.flag(site.capacity.closes_exactly);
  writer.u8(static_cast<std::uint8_t>(site.capacity.status));
  write_factors(writer, site.capacity.factors);

  write_connectivity(writer, site.connectivity);

  writer.count(site.failure_domains.nodes.size());
  for (const auto& node : site.failure_domains.nodes) {
    write_forest_node(writer, node);
  }
  writer.count(site.failure_domains.cyclic.size());
  for (const auto& domain : site.failure_domains.cyclic) {
    writer.text(domain.value());
  }
  writer.count(site.failure_domains.dangling.size());
  for (const auto& domain : site.failure_domains.dangling) {
    writer.text(domain.value());
  }
  writer.u8(static_cast<std::uint8_t>(site.failure_domains.status));
  write_factors(writer, site.failure_domains.factors);

  writer.count(site.maintenance.size());
  for (const auto& effect : site.maintenance) {
    write_maintenance_effect(writer, effect);
  }
  writer.count(site.obligations.size());
  for (const auto& verdict : site.obligations) {
    write_verdict(writer, verdict);
  }
  writer.count(site.conflicts.size());
  for (const auto& conflict : site.conflicts) {
    write_conflict(writer, conflict);
  }
  writer.count(site.deficits.size());
  for (const auto& deficit : site.deficits) {
    write_deficit(writer, deficit);
  }
  writer.count(site.decisions.size());
  for (const auto& decision : site.decisions) {
    write_decision(writer, decision);
  }
  write_factors(writer, site.factors);
}

Status read_composed_site(CanonicalReader& reader, ComposedSite& out) {
  Status status = read_strong_id(reader, out.site);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.epoch(out.epoch);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.generation(out.generation);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.incarnation(out.incarnation);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.i64(out.composed_at_ms);
  if (!is_ok(status)) {
    return status;
  }
  status = read_enum(reader, static_cast<std::uint8_t>(SiteLifecycle::STOPPED), out.lifecycle);
  if (!is_ok(status)) {
    return status;
  }
  status = read_enum(reader, static_cast<std::uint8_t>(Status::NO_AUTHORITATIVE_SOURCE), out.status);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.flag(out.membership_complete);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.flag(out.all_evidence_fresh);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.flag(out.any_conflict);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.flag(out.any_unknown_capacity);
  if (!is_ok(status)) {
    return status;
  }

  std::size_t count = 0;
  status = reader.count(count, limits::kMaxMemberDomains + 1);
  if (!is_ok(status)) {
    return status;
  }
  out.members.resize(count);
  for (auto& member : out.members) {
    status = read_member_state(reader, member);
    if (!is_ok(status)) {
      return status;
    }
  }

  status = reader.count(count, limits::kMaxDistinctResources + 1);
  if (!is_ok(status)) {
    return status;
  }
  out.ownership.resize(count);
  for (auto& attribution : out.ownership) {
    status = read_ownership(reader, attribution);
    if (!is_ok(status)) {
      return status;
    }
  }

  status = reader.count(count, limits::kMaxDistinctResources + 1);
  if (!is_ok(status)) {
    return status;
  }
  out.capacity.entries.resize(count);
  for (auto& entry : out.capacity.entries) {
    status = read_ledger_entry(reader, entry);
    if (!is_ok(status)) {
      return status;
    }
  }
  status = read_capacity(reader, out.capacity.total);
  if (!is_ok(status)) {
    return status;
  }
  status = read_capacity(reader, out.capacity.available);
  if (!is_ok(status)) {
    return status;
  }
  status = read_capacity(reader, out.capacity.excluded_by_maintenance);
  if (!is_ok(status)) {
    return status;
  }
  status = read_capacity(reader, out.capacity.indeterminate);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.flag(out.capacity.closes_exactly);
  if (!is_ok(status)) {
    return status;
  }
  status = read_enum(reader, static_cast<std::uint8_t>(Status::NO_AUTHORITATIVE_SOURCE),
                     out.capacity.status);
  if (!is_ok(status)) {
    return status;
  }
  status = read_factors(reader, out.capacity.factors);
  if (!is_ok(status)) {
    return status;
  }

  status = read_connectivity(reader, out.connectivity);
  if (!is_ok(status)) {
    return status;
  }

  status = reader.count(count, limits::kMaxDistinctFailureDomains + 1);
  if (!is_ok(status)) {
    return status;
  }
  out.failure_domains.nodes.resize(count);
  for (auto& node : out.failure_domains.nodes) {
    status = read_forest_node(reader, node);
    if (!is_ok(status)) {
      return status;
    }
  }
  status = reader.count(count, limits::kMaxDistinctFailureDomains + 1);
  if (!is_ok(status)) {
    return status;
  }
  out.failure_domains.cyclic.resize(count);
  for (auto& domain : out.failure_domains.cyclic) {
    status = read_strong_id(reader, domain);
    if (!is_ok(status)) {
      return status;
    }
  }
  status = reader.count(count, limits::kMaxDistinctFailureDomains + 1);
  if (!is_ok(status)) {
    return status;
  }
  out.failure_domains.dangling.resize(count);
  for (auto& domain : out.failure_domains.dangling) {
    status = read_strong_id(reader, domain);
    if (!is_ok(status)) {
      return status;
    }
  }
  status = read_enum(reader, static_cast<std::uint8_t>(Status::NO_AUTHORITATIVE_SOURCE),
                     out.failure_domains.status);
  if (!is_ok(status)) {
    return status;
  }
  status = read_factors(reader, out.failure_domains.factors);
  if (!is_ok(status)) {
    return status;
  }

  status = reader.count(count, limits::kMaxDistinctMaintenanceZones + 1);
  if (!is_ok(status)) {
    return status;
  }
  out.maintenance.resize(count);
  for (auto& effect : out.maintenance) {
    status = read_maintenance_effect(reader, effect);
    if (!is_ok(status)) {
      return status;
    }
  }

  status = reader.count(count, limits::kMaxDistinctObligations + 1);
  if (!is_ok(status)) {
    return status;
  }
  out.obligations.resize(count);
  for (auto& verdict : out.obligations) {
    status = read_verdict(reader, verdict);
    if (!is_ok(status)) {
      return status;
    }
  }

  status = reader.count(count, limits::kMaxConflicts + 1);
  if (!is_ok(status)) {
    return status;
  }
  out.conflicts.resize(count);
  for (auto& conflict : out.conflicts) {
    status = read_conflict(reader, conflict);
    if (!is_ok(status)) {
      return status;
    }
  }

  status = reader.count(count, limits::kMaxDeficits + 1);
  if (!is_ok(status)) {
    return status;
  }
  out.deficits.resize(count);
  for (auto& deficit : out.deficits) {
    status = read_deficit(reader, deficit);
    if (!is_ok(status)) {
      return status;
    }
  }

  status = reader.count(count, limits::kMaxDecisions + 1);
  if (!is_ok(status)) {
    return status;
  }
  out.decisions.resize(count);
  for (auto& decision : out.decisions) {
    status = read_decision(reader, decision);
    if (!is_ok(status)) {
      return status;
    }
  }
  return read_factors(reader, out.factors);
}

// ---------------------------------------------------------------------------
// Snapshot
// ---------------------------------------------------------------------------

void write_snapshot_body(CanonicalWriter& writer, const SiteSnapshot& snapshot) {
  writer.text(snapshot.site.value());
  writer.epoch(snapshot.epoch);
  writer.generation(snapshot.generation);
  writer.incarnation(snapshot.incarnation);
  writer.u64(snapshot.sequence);
  writer.digest(snapshot.site_digest);
  writer.i64(snapshot.published_at_ms);
  writer.u8(static_cast<std::uint8_t>(snapshot.lifecycle));
  writer.u8(static_cast<std::uint8_t>(snapshot.status));
  writer.flag(snapshot.complete);
  write_source_set(writer, snapshot.sources);
  write_factors(writer, snapshot.factors);
  write_composed_site(writer, snapshot.state);
}

Status read_snapshot_body(CanonicalReader& reader, SiteSnapshot& out) {
  Status status = read_strong_id(reader, out.site);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.epoch(out.epoch);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.generation(out.generation);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.incarnation(out.incarnation);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.u64(out.sequence);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.digest(out.site_digest);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.i64(out.published_at_ms);
  if (!is_ok(status)) {
    return status;
  }
  status = read_enum(reader, static_cast<std::uint8_t>(SiteLifecycle::STOPPED), out.lifecycle);
  if (!is_ok(status)) {
    return status;
  }
  status = read_enum(reader, static_cast<std::uint8_t>(Status::NO_AUTHORITATIVE_SOURCE), out.status);
  if (!is_ok(status)) {
    return status;
  }
  status = reader.flag(out.complete);
  if (!is_ok(status)) {
    return status;
  }
  status = read_source_set(reader, out.sources);
  if (!is_ok(status)) {
    return status;
  }
  status = read_factors(reader, out.factors);
  if (!is_ok(status)) {
    return status;
  }
  return read_composed_site(reader, out.state);
}

}  // namespace site_fabric::internal
