// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "site_fabric/site_fabric.hpp"
#include "site_fabric/synthetic.hpp"
#include "test_support.hpp"

using namespace site_fabric;

namespace {

/// A minimal but complete site: two racks and one external link, so every
/// aggregate has a source and the site can actually be judged CURRENT.
std::vector<MemberDomainDeclaration> basic_site() {
  std::vector<MemberDomainDeclaration> declarations;
  declarations.push_back(sftest::simple_rack_declaration("rack-a", "r-a", 1000, 1000000, 60000));
  declarations.push_back(sftest::simple_rack_declaration("rack-b", "r-b", 2000, 1000000, 60000));
  sftest::add_external_link(declarations[0], "link-a", 5000, 1000000);
  sftest::add_gateway(declarations[0], "gw-a", 5000, 1000000);
  for (auto& declaration : declarations) {
    (void)declaration.seal();
  }
  return declarations;
}

void add_shared_link(MemberDomainDeclaration& declaration, const std::string& id, bool external,
                     std::uint64_t rate, std::int64_t observed) {
  SharedLinkClaim claim;
  claim.id = LinkId::unchecked(id);
  claim.external = external;
  claim.generation = Generation::initial();
  claim.capacity = external ? CapacityVector(CapacityValue(rate), CapacityValue(rate),
                                             CapacityValue::unknown())
                            : CapacityVector(CapacityValue::unknown(), CapacityValue::unknown(),
                                             CapacityValue(rate));
  claim.state = ConnectivityState::UP;
  claim.evidence = Evidence::reported(observed, 60000);
  declaration.shared_links.push_back(std::move(claim));
}

}  // namespace

SF_TEST(composition, all_members_current_yields_current) {
  const auto declarations = basic_site();
  SiteComposer composer;
  ComposedSite site;
  const Status status = composer.compose(sftest::input_over(declarations, 1000000), site);
  SF_CHECK_EQ(Status::COMPOSED, status);
  SF_CHECK_EQ(SiteLifecycle::CURRENT, site.lifecycle);
  SF_CHECK_EQ(Status::CURRENT, site.status);
  SF_CHECK(site.membership_complete);
  SF_CHECK(site.all_evidence_fresh);
  SF_CHECK(site.conflicts.empty());
  SF_CHECK(site.deficits.empty());
  SF_CHECK_EQ(std::size_t(2), site.members.size());
  for (const auto& member : site.members) {
    SF_CHECK_EQ(MemberLifecycle::CURRENT, member.lifecycle);
  }
}

SF_TEST(composition, missing_required_member_is_incomplete) {
  auto declarations = basic_site();
  CompositionInput input = sftest::input_over(declarations, 1000000);
  ExpectedMember absent;
  absent.domain = MemberDomainKey::rack("rack-never");
  absent.required = true;
  input.expectation.members.push_back(absent);
  SF_REQUIRE(is_ok(input.expectation.canonicalize()));

  SiteComposer composer;
  ComposedSite site;
  SF_CHECK_EQ(Status::COMPOSED, composer.compose(input, site));
  SF_CHECK_EQ(SiteLifecycle::INCOMPLETE, site.lifecycle);
  SF_CHECK(!site.membership_complete);
  SF_REQUIRE(!site.deficits.empty());
  bool found = false;
  for (const auto& deficit : site.deficits) {
    if (deficit.code == Status::MEMBERSHIP_MISSING &&
        deficit.domain == MemberDomainKey::rack("rack-never")) {
      found = true;
    }
  }
  SF_CHECK(found);
  const MemberDomainState* state = site.find_member(MemberDomainKey::rack("rack-never"));
  SF_REQUIRE(state != nullptr);
  SF_CHECK_EQ(MemberLifecycle::ABSENT, state->lifecycle);
}

SF_TEST(composition, every_required_member_absent_is_partitioned) {
  std::vector<MemberDomainDeclaration> none;
  auto declarations = basic_site();
  CompositionInput input = sftest::input_over(none, 1000000);
  input.expectation = sftest::expectation_over(declarations);
  SiteComposer composer;
  ComposedSite site;
  SF_CHECK_EQ(Status::COMPOSED, composer.compose(input, site));
  SF_CHECK_EQ(SiteLifecycle::PARTITIONED, site.lifecycle);
}

SF_TEST(composition, pinned_expectation_generation_mismatch_conflicts) {
  auto declarations = basic_site();
  CompositionInput input = sftest::input_over(declarations, 1000000);
  for (auto& member : input.expectation.members) {
    member.pinned = true;
    member.generation = Generation(9);
    member.digest = internal::digest_text("expected");
  }
  SF_REQUIRE(is_ok(input.expectation.canonicalize()));

  SiteComposer composer;
  ComposedSite site;
  SF_CHECK_EQ(Status::COMPOSED, composer.compose(input, site));
  SF_CHECK_EQ(SiteLifecycle::CONFLICTING, site.lifecycle);
  SF_CHECK(site.any_conflict);
  const MemberDomainState* state = site.find_member(MemberDomainKey::rack("rack-a"));
  SF_REQUIRE(state != nullptr);
  SF_CHECK_EQ(MemberLifecycle::CONFLICTING, state->lifecycle);
  SF_CHECK_EQ(Status::GENERATION_MISMATCH, state->status);
}

SF_TEST(composition, site_mismatch_is_incompatible) {
  auto declarations = basic_site();
  declarations[0].site = SiteId::unchecked("site-elsewhere");
  SF_REQUIRE(is_ok(declarations[0].seal()));
  SF_CHECK(declarations[0].seal() == Status::OK);
  SiteComposer composer;
  ComposedSite site;
  SF_CHECK_EQ(Status::COMPOSED, composer.compose(sftest::input_over(declarations, 1000000), site));
  const MemberDomainState* state = site.find_member(MemberDomainKey::rack("rack-a"));
  SF_REQUIRE(state != nullptr);
  SF_CHECK_EQ(MemberLifecycle::INCOMPATIBLE, state->lifecycle);
  SF_CHECK_EQ(Status::SITE_MISMATCH, state->status);
  SF_CHECK_EQ(SiteLifecycle::INCOMPLETE, site.lifecycle);
}

SF_TEST(composition, schema_mismatch_is_incompatible) {
  auto declarations = basic_site();
  CompositionInput input = sftest::input_over(declarations, 1000000);
  input.publications[0].schema = SchemaVersion{kModelSchemaMajor + 1, 0};
  SiteComposer composer;
  ComposedSite site;
  SF_CHECK_EQ(Status::COMPOSED, composer.compose(input, site));
  const MemberDomainState* state = site.find_member(MemberDomainKey::rack("rack-a"));
  SF_REQUIRE(state != nullptr);
  SF_CHECK_EQ(Status::SCHEMA_INCOMPATIBLE, state->status);
}

SF_TEST(composition, digest_mismatch_is_conflicting) {
  auto declarations = basic_site();
  CompositionInput input = sftest::input_over(declarations, 1000000);
  input.publications[0].digest = internal::digest_text("not the model");
  SiteComposer composer;
  ComposedSite site;
  SF_CHECK_EQ(Status::COMPOSED, composer.compose(input, site));
  const MemberDomainState* state = site.find_member(MemberDomainKey::rack("rack-a"));
  SF_REQUIRE(state != nullptr);
  SF_CHECK_EQ(MemberLifecycle::CONFLICTING, state->lifecycle);
  SF_CHECK_EQ(Status::DIGEST_MISMATCH_DECLARATION, state->status);
  SF_CHECK_EQ(SiteLifecycle::CONFLICTING, site.lifecycle);
}

SF_TEST(composition, stale_evidence_never_becomes_current) {
  auto declarations = basic_site();
  declarations[1].evidence = Evidence::reported(500000, 1000);
  declarations[1].racks[0].evidence = declarations[1].evidence;
  SF_REQUIRE(is_ok(declarations[1].seal()));
  SiteComposer composer;
  ComposedSite site;
  SF_CHECK_EQ(Status::COMPOSED, composer.compose(sftest::input_over(declarations, 1000000), site));
  const MemberDomainState* state = site.find_member(MemberDomainKey::rack("rack-b"));
  SF_REQUIRE(state != nullptr);
  SF_CHECK_EQ(MemberLifecycle::STALE, state->lifecycle);
  SF_CHECK_EQ(Status::EVIDENCE_STALE, state->status);
  SF_CHECK_EQ(SiteLifecycle::INCOMPLETE, site.lifecycle);
  SF_CHECK(!site.membership_complete);
}

SF_TEST(composition, synthetic_evidence_never_yields_current) {
  auto declarations = basic_site();
  for (auto& declaration : declarations) {
    declaration.evidence.provenance = Provenance::SYNTHETIC;
    declaration.racks[0].evidence.provenance = Provenance::SYNTHETIC;
    SF_REQUIRE(is_ok(declaration.seal()));
  }
  SiteComposer composer;
  ComposedSite site;
  SF_CHECK_EQ(Status::COMPOSED, composer.compose(sftest::input_over(declarations, 1000000), site));
  SF_CHECK_NE(SiteLifecycle::CURRENT, site.lifecycle);
  SF_CHECK_EQ(Status::EVIDENCE_SYNTHETIC_ONLY, site.members.front().status);
}

SF_TEST(composition, recomposition_is_deterministic) {
  std::vector<MemberDomainDeclaration> declarations;
  declarations.push_back(sftest::simple_rack_declaration("rack-a", "r-a", 1000, 1000000, 60000));
  declarations.push_back(sftest::simple_rack_declaration("rack-b", "r-b", 2000, 1000000, 60000));
  add_shared_link(declarations[0], "link-0", true, 5000, 1000000);
  add_shared_link(declarations[1], "link-0", true, 5000, 1000000);
  for (auto& declaration : declarations) {
    SF_REQUIRE(is_ok(declaration.seal()));
  }

  SiteComposer composer;
  ComposedSite first;
  SF_REQUIRE(is_ok(composer.compose(sftest::input_over(declarations, 1000000), first)));

  ComposedSite second;
  SF_REQUIRE(is_ok(composer.compose(sftest::input_over(declarations, 1000000), second)));
  SF_CHECK(first.compute_digest() == second.compute_digest());

  // Arrival order must not matter.
  for (int rotation = 1; rotation < 3; ++rotation) {
    CompositionInput input = sftest::input_over(declarations, 1000000);
    std::rotate(input.publications.begin(), input.publications.begin() + rotation,
                input.publications.end());
    ComposedSite rotated;
    SF_REQUIRE(is_ok(composer.compose(input, rotated)));
    SF_CHECK(first.compute_digest() == rotated.compute_digest());
    SF_CHECK_EQ(first.capacity.total.to_string(), rotated.capacity.total.to_string());
  }

  // A different evaluation instant is a different site state.
  ComposedSite later;
  SF_REQUIRE(is_ok(composer.compose(sftest::input_over(declarations, 1000001), later)));
  SF_CHECK(!(first.compute_digest() == later.compute_digest()));
}

SF_TEST(composition, site_identity_is_canonical) {
  auto declarations = basic_site();
  SiteComposer composer;
  ComposedSite site;
  SF_REQUIRE(is_ok(composer.compose(sftest::input_over(declarations, 1000000), site)));

  CompositionInput other = sftest::input_over(declarations, 1000000);
  other.site = SiteId::unchecked("site-beta");
  other.expectation.site = other.site;
  for (auto& publication : other.publications) {
    publication.site = other.site;
  }
  ComposedSite beta;
  SF_REQUIRE(is_ok(composer.compose(other, beta)));
  SF_CHECK(!(site.compute_digest() == beta.compute_digest()));
}

SF_TEST(composition, overlapping_ownership_is_a_conflict) {
  auto declarations = basic_site();
  MemberDomainDeclaration second =
      sftest::simple_rack_declaration("rack-c", "r-a", 9999, 1000000, 60000);
  declarations.push_back(second);
  SF_REQUIRE(is_ok(declarations[0].seal()));

  SiteComposer composer;
  ComposedSite site;
  SF_CHECK_EQ(Status::COMPOSED, composer.compose(sftest::input_over(declarations, 1000000), site));
  SF_CHECK_EQ(SiteLifecycle::CONFLICTING, site.lifecycle);

  const ResourceAttribution* attribution =
      site.find_ownership(ResourceKey::rack("r-a"));
  SF_REQUIRE(attribution != nullptr);
  SF_CHECK_EQ(Status::OWNERSHIP_OVERLAP, attribution->status);
  SF_CHECK_EQ(std::size_t(2), attribution->attestors.size());
  SF_CHECK_EQ(std::size_t(2), attribution->distinct_contents);

  bool found_conflict = false;
  for (const auto& conflict : site.conflicts) {
    if (conflict.code == Status::OWNERSHIP_OVERLAP) {
      found_conflict = true;
      SF_CHECK_EQ(ResourceKey::rack("r-a"), conflict.subject);
    }
  }
  SF_CHECK(found_conflict);

  // A conflicting resource contributes nothing to the aggregate, so only the
  // rack the two domains agree about is counted.
  SF_CHECK(site.capacity.total.internal.known);
  SF_CHECK_EQ(std::uint64_t(2000), site.capacity.total.internal.bps);
  const CapacityLedgerEntry* conflicted = site.capacity.find(ResourceKey::rack("r-a"));
  SF_REQUIRE(conflicted != nullptr);
  SF_CHECK(!conflicted->counted_in_total);
}

SF_TEST(composition, identical_claims_from_two_domains_count_once) {
  auto declarations = basic_site();
  declarations.push_back(sftest::simple_rack_declaration_for(
      MemberDomainKey::pod("pod-a"), "r-a", 1000, 1000000, 60000));
  SF_REQUIRE(is_ok(declarations.back().seal()));

  SiteComposer composer;
  ComposedSite site;
  SF_CHECK_EQ(Status::COMPOSED, composer.compose(sftest::input_over(declarations, 1000000), site));
  const ResourceAttribution* attribution = site.find_ownership(ResourceKey::rack("r-a"));
  SF_REQUIRE(attribution != nullptr);
  SF_CHECK_EQ(Status::OK, attribution->status);
  SF_CHECK_EQ(std::size_t(2), attribution->attestors.size());
  SF_CHECK_EQ(std::size_t(1), attribution->distinct_contents);
  // 1000 for r-a plus 2000 for r-b, not 1000 twice.
  SF_CHECK_EQ(std::uint64_t(3000), site.capacity.total.internal.bps);
}

SF_TEST(composition, shared_links_are_counted_once) {
  std::vector<MemberDomainDeclaration> declarations;
  for (int index = 0; index < 3; ++index) {
    MemberDomainDeclaration declaration = sftest::simple_rack_declaration(
        "rack-" + std::to_string(index), "r-" + std::to_string(index), 100, 1000000, 60000);
    add_shared_link(declaration, "link-shared", true, 7000, 1000000);
    SF_REQUIRE(is_ok(declaration.seal()));
    declarations.push_back(std::move(declaration));
  }

  SiteComposer composer;
  ComposedSite site;
  SF_CHECK_EQ(Status::COMPOSED, composer.compose(sftest::input_over(declarations, 1000000), site));
  const ResourceAttribution* attribution =
      site.find_ownership(ResourceKey::shared_link("link-shared"));
  SF_REQUIRE(attribution != nullptr);
  SF_CHECK_EQ(std::size_t(3), attribution->attestors.size());
  SF_CHECK_EQ(Status::OK, attribution->status);
  SF_CHECK_EQ(std::uint64_t(7000), site.capacity.total.ingress.bps);
  SF_CHECK_EQ(std::uint64_t(7000), site.capacity.total.egress.bps);
}

SF_TEST(composition, disagreeing_shared_link_capacity_is_an_accounting_conflict) {
  std::vector<MemberDomainDeclaration> declarations;
  for (int index = 0; index < 2; ++index) {
    MemberDomainDeclaration declaration = sftest::simple_rack_declaration(
        "rack-" + std::to_string(index), "r-" + std::to_string(index), 100, 1000000, 60000);
    add_shared_link(declaration, "link-shared", true, index == 0 ? 7000 : 3000, 1000000);
    SF_REQUIRE(is_ok(declaration.seal()));
    declarations.push_back(std::move(declaration));
  }

  SiteComposer composer;
  ComposedSite site;
  SF_CHECK_EQ(Status::COMPOSED, composer.compose(sftest::input_over(declarations, 1000000), site));
  const ResourceAttribution* attribution =
      site.find_ownership(ResourceKey::shared_link("link-shared"));
  SF_REQUIRE(attribution != nullptr);
  SF_CHECK_EQ(Status::ACCOUNTING_CONFLICT, attribution->status);
  SF_CHECK_EQ(SiteLifecycle::CONFLICTING, site.lifecycle);
  // The conflicted link is not folded into the ingress total.
  SF_CHECK(!site.capacity.total.ingress.known);
  const CapacityLedgerEntry* entry = site.capacity.find(ResourceKey::shared_link("link-shared"));
  SF_REQUIRE(entry != nullptr);
  SF_CHECK(!entry->counted_in_total);
}

SF_TEST(composition, every_authoritative_field_resolves_to_sources) {
  const auto declarations = basic_site();
  SiteComposer composer;
  ComposedSite site;
  SF_REQUIRE(is_ok(composer.compose(sftest::input_over(declarations, 1000000), site)));

  for (const auto& member : site.members) {
    SF_CHECK(member.source.valid());
    SF_CHECK(member.source.domain == member.domain);
  }
  for (const auto& attribution : site.ownership) {
    SF_CHECK(!attribution.attestors.empty());
  }
  for (const auto& entry : site.capacity.entries) {
    if (entry.counted_in_total) {
      SF_CHECK(!entry.attestors.empty());
    }
  }
  SF_CHECK(!site.decisions.empty());
  for (const auto& decision : site.decisions) {
    if (decision.id.kind == DecisionKind::SITE_LIFECYCLE) {
      SF_CHECK(!decision.sources.empty());
    }
  }
}

SF_TEST(composition, unexpected_member_is_recorded_when_rejected) {
  auto declarations = basic_site();
  CompositionInput input = sftest::input_over(declarations, 1000000);
  input.expectation.members.erase(input.expectation.members.begin());
  input.expectation.reject_unexpected = true;
  SF_REQUIRE(is_ok(input.expectation.canonicalize()));

  SiteComposer composer;
  ComposedSite site;
  SF_CHECK_EQ(Status::COMPOSED, composer.compose(input, site));
  const MemberDomainState* state = site.find_member(MemberDomainKey::rack("rack-a"));
  SF_REQUIRE(state != nullptr);
  SF_CHECK_EQ(Status::MEMBERSHIP_UNEXPECTED, state->status);
}

SF_TEST(composition, retirement_is_terminal) {
  auto declarations = basic_site();
  CompositionInput input = sftest::input_over(declarations, 1000000);
  MemberRetirementRecord retirement;
  retirement.domain = MemberDomainKey::rack("rack-a");
  retirement.generation = Generation(2);
  retirement.incarnation = input.incarnation;
  retirement.received_at_ms = 1000000;
  retirement.reason = "decommissioned";
  input.retirements.push_back(retirement);

  SiteComposer composer;
  ComposedSite site;
  SF_CHECK_EQ(Status::COMPOSED, composer.compose(input, site));
  const MemberDomainState* state = site.find_member(MemberDomainKey::rack("rack-a"));
  SF_REQUIRE(state != nullptr);
  SF_CHECK_EQ(MemberLifecycle::RETIRED, state->lifecycle);
  SF_CHECK_EQ(Status::ALREADY_RETIRED, state->status);
    SF_CHECK_EQ(SiteLifecycle::INCOMPLETE, site.lifecycle);
}

SF_TEST(composition, revoke_beats_everything) {
  auto declarations = basic_site();
  CompositionInput input = sftest::input_over(declarations, 1000000);
  input.revoked.push_back(MemberDomainKey::rack("rack-b"));
  SiteComposer composer;
  ComposedSite site;
  SF_CHECK_EQ(Status::COMPOSED, composer.compose(input, site));
  const MemberDomainState* state = site.find_member(MemberDomainKey::rack("rack-b"));
  SF_REQUIRE(state != nullptr);
  SF_CHECK_EQ(MemberLifecycle::REVOKED, state->lifecycle);
  SF_CHECK_EQ(Status::LEASE_REVOKED, state->status);
}

SF_TEST(composition, populated_ownership_matches_authoritative_claims) {
  auto declarations = basic_site();
  add_shared_link(declarations[0], "link-0", false, 100, 1000000);
  for (auto& declaration : declarations) {
    SF_REQUIRE(is_ok(declaration.seal()));
  }
  SiteComposer composer;
  ComposedSite site;
  SF_REQUIRE(is_ok(composer.compose(sftest::input_over(declarations, 1000000), site)));
  SF_CHECK(site.find_ownership(ResourceKey::rack("r-a")) != nullptr);
  SF_CHECK(site.find_ownership(ResourceKey::rack("r-b")) != nullptr);
  SF_CHECK(site.find_ownership(ResourceKey::shared_link("link-0")) != nullptr);
  SF_CHECK(site.find_ownership(ResourceKey::shared_link("link-a")) != nullptr);
  SF_CHECK(site.find_ownership(ResourceKey::gateway("gw-a")) != nullptr);
  SF_CHECK_EQ(std::size_t(5), site.ownership.size());
}
