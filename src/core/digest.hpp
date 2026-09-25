// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Canonical encoders and content digests.
//
// Every type that carries a digest is encoded in exactly one place, so a
// digest can never depend on which call site computed it. The encoders are
// order-sensitive: callers canonicalise their collections before encoding, and
// the encoders assert that by writing the size first.

#ifndef SITE_FABRIC_CORE_DIGEST_HPP
#define SITE_FABRIC_CORE_DIGEST_HPP

#include "core/canonical.hpp"
#include "site_fabric/authority.hpp"
#include "site_fabric/composition.hpp"
#include "site_fabric/member.hpp"
#include "site_fabric/snapshot.hpp"

namespace site_fabric::internal {

void write_member_domain_key(CanonicalWriter& writer, const MemberDomainKey& key);
void write_resource_key(CanonicalWriter& writer, const ResourceKey& key);
void write_evidence(CanonicalWriter& writer, const Evidence& evidence);
void write_source_set(CanonicalWriter& writer, const SourceSet& sources);
void write_capacity_value(CanonicalWriter& writer, const CapacityValue& value);
void write_capacity(CanonicalWriter& writer, const CapacityVector& vector);
void write_factors(CanonicalWriter& writer, const Factors& factors);

void write_claim(CanonicalWriter& writer, const RackClaim& claim);
void write_claim(CanonicalWriter& writer, const PodClaim& claim);
void write_claim(CanonicalWriter& writer, const ClusterClaim& claim);
void write_claim(CanonicalWriter& writer, const SharedLinkClaim& claim);
void write_claim(CanonicalWriter& writer, const GatewayClaim& claim);
void write_claim(CanonicalWriter& writer, const CapacityContribution& claim);
void write_claim(CanonicalWriter& writer, const FailureDomainClaim& claim);
void write_claim(CanonicalWriter& writer, const MaintenanceZone& claim);
void write_claim(CanonicalWriter& writer, const ProtectedObligation& claim);

/// Content digest of a single claim, including its record generation but not
/// its own record digest.
[[nodiscard]] Digest claim_digest(const RackClaim& claim);
[[nodiscard]] Digest claim_digest(const PodClaim& claim);
[[nodiscard]] Digest claim_digest(const ClusterClaim& claim);
[[nodiscard]] Digest claim_digest(const SharedLinkClaim& claim);
[[nodiscard]] Digest claim_digest(const GatewayClaim& claim);
[[nodiscard]] Digest claim_digest(const CapacityContribution& claim);
[[nodiscard]] Digest claim_digest(const FailureDomainClaim& claim);
[[nodiscard]] Digest claim_digest(const MaintenanceZone& claim);
[[nodiscard]] Digest claim_digest(const ProtectedObligation& claim);

/// Digest of everything a declaration asserts, excluding the declaration's own
/// digest field and the self-referential evidence source.
void write_declaration_content(CanonicalWriter& writer,
                               const MemberDomainDeclaration& declaration);
[[nodiscard]] Digest declaration_content_digest(const MemberDomainDeclaration& declaration);

void write_publication(CanonicalWriter& writer, const MemberPublicationRecord& record);
[[nodiscard]] Digest publication_digest(const MemberPublicationRecord& record);

void write_authority_token(CanonicalWriter& writer, const AuthorityToken& token);

void write_composed_site(CanonicalWriter& writer, const ComposedSite& site);
[[nodiscard]] Digest composed_site_digest(const ComposedSite& site);
[[nodiscard]] Digest snapshot_envelope_digest(const SiteSnapshot& snapshot);
[[nodiscard]] Digest decision_value_digest(const SiteDecision& decision);

// --- Full codecs -----------------------------------------------------------
//
// These are the read/write pairs used by the wire protocol and by the store.
// Every reader validates bounds before allocating and returns a distinguishing
// status rather than a partially populated value.

// Primitives shared by every decoder.
Status read_identifier(CanonicalReader& reader, std::string& out);
Status read_member_domain_key(CanonicalReader& reader, MemberDomainKey& out);
Status read_resource_key(CanonicalReader& reader, ResourceKey& out);
Status read_capacity_value(CanonicalReader& reader, CapacityValue& out);
Status read_capacity(CanonicalReader& reader, CapacityVector& out);
Status read_evidence(CanonicalReader& reader, Evidence& out);
Status read_factors(CanonicalReader& reader, Factors& out);
Status read_source_set(CanonicalReader& reader, SourceSet& out);

/// Reads a validated identity. The text is validated before the StrongId is
/// constructed, so an unchecked identity can never enter the model from the
/// wire or from disk.
template <class Tag>
[[nodiscard]] Status read_strong_id(CanonicalReader& reader, StrongId<Tag>& out) {
  std::string text;
  const Status status = read_identifier(reader, text);
  if (!is_ok(status)) {
    return status;
  }
  out = StrongId<Tag>::unchecked(std::move(text));
  return Status::OK;
}

void write_declaration(CanonicalWriter& writer, const MemberDomainDeclaration& declaration);
Status read_declaration(CanonicalReader& reader, MemberDomainDeclaration& out);

void write_expectation(CanonicalWriter& writer, const SiteExpectation& expectation);
Status read_expectation(CanonicalReader& reader, SiteExpectation& out);

void write_publication_record(CanonicalWriter& writer, const MemberPublicationRecord& record);
Status read_publication_record(CanonicalReader& reader, MemberPublicationRecord& out);

void write_retirement_record(CanonicalWriter& writer, const MemberRetirementRecord& record);
Status read_retirement_record(CanonicalReader& reader, MemberRetirementRecord& out);

void write_authority_token(CanonicalWriter& writer, const AuthorityToken& token);
Status read_authority_token(CanonicalReader& reader, AuthorityToken& out);

void write_snapshot_body(CanonicalWriter& writer, const SiteSnapshot& snapshot);
Status read_snapshot_body(CanonicalReader& reader, SiteSnapshot& out);

void write_composed_site(CanonicalWriter& writer, const ComposedSite& site);
Status read_composed_site(CanonicalReader& reader, ComposedSite& out);

}  // namespace site_fabric::internal

#endif  // SITE_FABRIC_CORE_DIGEST_HPP
