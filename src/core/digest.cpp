// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "core/digest.hpp"

#include <string>

namespace site_fabric::internal {

void write_member_domain_key(CanonicalWriter& writer, const MemberDomainKey& key) {
  writer.u8(static_cast<std::uint8_t>(key.kind));
  writer.text(key.id);
}

void write_resource_key(CanonicalWriter& writer, const ResourceKey& key) {
  writer.u8(static_cast<std::uint8_t>(key.kind));
  writer.text(key.id);
}

void write_evidence(CanonicalWriter& writer, const Evidence& evidence) {
  writer.u8(static_cast<std::uint8_t>(evidence.provenance));
  writer.i64(evidence.observed_at_ms);
  writer.u64(evidence.ttl_ms);
  writer.digest(evidence.payload_digest);
}

void write_source_set(CanonicalWriter& writer, const SourceSet& sources) {
  writer.count(sources.size());
  for (const auto& ref : sources.items()) {
    writer.source_ref(ref);
  }
}

void write_capacity_value(CanonicalWriter& writer, const CapacityValue& value) {
  writer.flag(value.known);
  writer.u64(value.known ? value.bps : 0);
}

void write_capacity(CanonicalWriter& writer, const CapacityVector& vector) {
  write_capacity_value(writer, vector.ingress);
  write_capacity_value(writer, vector.egress);
  write_capacity_value(writer, vector.internal);
}

void write_factors(CanonicalWriter& writer, const Factors& factors) {
  writer.count(factors.size());
  for (const auto& factor : factors.items()) {
    writer.text(factor);
  }
}

void write_claim(CanonicalWriter& writer, const RackClaim& claim) {
  writer.text(claim.id.value());
  writer.generation(claim.generation);
  write_capacity(writer, claim.local_capacity);
  writer.u8(static_cast<std::uint8_t>(claim.uplink));
  write_evidence(writer, claim.evidence);
}

void write_claim(CanonicalWriter& writer, const PodClaim& claim) {
  writer.text(claim.id.value());
  writer.generation(claim.generation);
  writer.count(claim.racks.size());
  for (const auto& rack : claim.racks) {
    writer.text(rack.value());
  }
  write_evidence(writer, claim.evidence);
}

void write_claim(CanonicalWriter& writer, const ClusterClaim& claim) {
  writer.text(claim.id.value());
  writer.generation(claim.generation);
  writer.count(claim.pods.size());
  for (const auto& pod : claim.pods) {
    writer.text(pod.value());
  }
  writer.count(claim.racks.size());
  for (const auto& rack : claim.racks) {
    writer.text(rack.value());
  }
  write_evidence(writer, claim.evidence);
}

void write_claim(CanonicalWriter& writer, const SharedLinkClaim& claim) {
  writer.text(claim.id.value());
  writer.flag(claim.external);
  writer.generation(claim.generation);
  write_capacity(writer, claim.capacity);
  writer.u8(static_cast<std::uint8_t>(claim.state));
  write_evidence(writer, claim.evidence);
}

void write_claim(CanonicalWriter& writer, const GatewayClaim& claim) {
  writer.text(claim.id.value());
  writer.generation(claim.generation);
  write_capacity(writer, claim.capacity);
  writer.u8(static_cast<std::uint8_t>(claim.state));
  write_evidence(writer, claim.evidence);
}

void write_claim(CanonicalWriter& writer, const CapacityContribution& claim) {
  write_resource_key(writer, claim.owner);
  writer.u8(static_cast<std::uint8_t>(claim.scope));
  writer.generation(claim.generation);
  write_capacity(writer, claim.capacity);
  write_evidence(writer, claim.evidence);
}

void write_claim(CanonicalWriter& writer, const FailureDomainClaim& claim) {
  writer.text(claim.id.value());
  writer.u8(static_cast<std::uint8_t>(claim.domain_class));
  writer.optional(claim.parent.has_value());
  if (claim.parent.has_value()) {
    writer.text(claim.parent->value());
  }
  writer.count(claim.members.size());
  for (const auto& member : claim.members) {
    write_resource_key(writer, member);
  }
  writer.generation(claim.generation);
  write_evidence(writer, claim.evidence);
}

void write_claim(CanonicalWriter& writer, const MaintenanceZone& claim) {
  writer.text(claim.id.value());
  writer.u8(static_cast<std::uint8_t>(claim.state));
  writer.i64(claim.window_start_ms);
  writer.i64(claim.window_end_ms);
  writer.count(claim.domains.size());
  for (const auto& domain : claim.domains) {
    writer.text(domain.value());
  }
  writer.count(claim.resources.size());
  for (const auto& resource : claim.resources) {
    write_resource_key(writer, resource);
  }
  writer.generation(claim.generation);
  write_evidence(writer, claim.evidence);
}

void write_claim(CanonicalWriter& writer, const ProtectedObligation& claim) {
  writer.text(claim.id.value());
  writer.u8(static_cast<std::uint8_t>(claim.kind));
  writer.optional(claim.scope_domain.has_value());
  if (claim.scope_domain.has_value()) {
    writer.text(claim.scope_domain->value());
  }
  writer.optional(claim.scope_resource.has_value());
  if (claim.scope_resource.has_value()) {
    write_resource_key(writer, *claim.scope_resource);
  }
  writer.u64(claim.required);
  writer.flag(claim.protected_from_maintenance);
  writer.generation(claim.generation);
  write_evidence(writer, claim.evidence);
}

#define SITE_FABRIC_CLAIM_DIGEST(type)                            \
  Digest claim_digest(const type& claim) {                        \
    CanonicalWriter writer;                                       \
    write_claim(writer, claim);                                   \
    return writer.finish_digest();                                \
  }

SITE_FABRIC_CLAIM_DIGEST(RackClaim)
SITE_FABRIC_CLAIM_DIGEST(PodClaim)
SITE_FABRIC_CLAIM_DIGEST(ClusterClaim)
SITE_FABRIC_CLAIM_DIGEST(SharedLinkClaim)
SITE_FABRIC_CLAIM_DIGEST(GatewayClaim)
SITE_FABRIC_CLAIM_DIGEST(CapacityContribution)
SITE_FABRIC_CLAIM_DIGEST(FailureDomainClaim)
SITE_FABRIC_CLAIM_DIGEST(MaintenanceZone)
SITE_FABRIC_CLAIM_DIGEST(ProtectedObligation)

#undef SITE_FABRIC_CLAIM_DIGEST

void write_declaration_content(CanonicalWriter& writer,
                               const MemberDomainDeclaration& declaration) {
  write_member_domain_key(writer, declaration.domain);
  writer.text(declaration.site.value());
  writer.generation(declaration.generation);
  writer.schema(declaration.schema);
  writer.epoch(declaration.observed_epoch);
  write_evidence(writer, declaration.evidence);

  writer.count(declaration.clusters.size());
  for (const auto& claim : declaration.clusters) {
    write_claim(writer, claim);
  }
  writer.count(declaration.pods.size());
  for (const auto& claim : declaration.pods) {
    write_claim(writer, claim);
  }
  writer.count(declaration.racks.size());
  for (const auto& claim : declaration.racks) {
    write_claim(writer, claim);
  }
  writer.count(declaration.shared_links.size());
  for (const auto& claim : declaration.shared_links) {
    write_claim(writer, claim);
  }
  writer.count(declaration.gateways.size());
  for (const auto& claim : declaration.gateways) {
    write_claim(writer, claim);
  }
  writer.count(declaration.capacity.size());
  for (const auto& claim : declaration.capacity) {
    write_claim(writer, claim);
  }
  writer.count(declaration.failure_domains.size());
  for (const auto& claim : declaration.failure_domains) {
    write_claim(writer, claim);
  }
  writer.count(declaration.maintenance_zones.size());
  for (const auto& claim : declaration.maintenance_zones) {
    write_claim(writer, claim);
  }
  writer.count(declaration.obligations.size());
  for (const auto& claim : declaration.obligations) {
    write_claim(writer, claim);
  }
}

Digest declaration_content_digest(const MemberDomainDeclaration& declaration) {
  CanonicalWriter writer;
  write_declaration_content(writer, declaration);
  return writer.finish_digest();
}

void write_publication(CanonicalWriter& writer, const MemberPublicationRecord& record) {
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
    write_declaration_content(writer, record.declaration);
  }
}

Digest publication_digest(const MemberPublicationRecord& record) {
  CanonicalWriter writer;
  write_publication(writer, record);
  return writer.finish_digest();
}

Digest composed_site_digest(const ComposedSite& site) {
  CanonicalWriter writer;
  write_composed_site(writer, site);
  return writer.finish_digest();
}

Digest snapshot_envelope_digest(const SiteSnapshot& snapshot) {
  CanonicalWriter writer;
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
  return writer.finish_digest();
}

}  // namespace site_fabric::internal
