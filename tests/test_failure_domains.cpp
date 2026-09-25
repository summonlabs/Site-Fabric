// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>
#include <vector>

#include "site_fabric/site_fabric.hpp"
#include "test_support.hpp"

using namespace site_fabric;

namespace {

MemberDomainDeclaration domain_with_domain(const std::string& domain_id, const std::string& fd_id,
                                           const std::string& parent, bool physical,
                                           const std::vector<ResourceKey>& members,
                                           std::int64_t observed) {
  MemberDomainDeclaration declaration;
  declaration.domain = MemberDomainKey::rack(domain_id);
  declaration.site = SiteId::unchecked("site-alpha");
  declaration.generation = Generation::initial();
  declaration.observed_epoch = Epoch::initial();
  declaration.evidence = Evidence::reported(observed, 60000);

  FailureDomainClaim claim;
  claim.id = FailureDomainId::unchecked(fd_id);
  claim.domain_class = physical ? FailureDomainClass::PHYSICAL
                                : FailureDomainClass::ADMINISTRATIVE;
  if (!parent.empty()) {
    claim.parent = FailureDomainId::unchecked(parent);
  }
  claim.members = members;
  claim.generation = Generation::initial();
  claim.evidence = declaration.evidence;
  declaration.failure_domains.push_back(std::move(claim));

  RackClaim rack;
  rack.id = RackId::unchecked("rack-" + domain_id);
  rack.generation = Generation::initial();
  rack.local_capacity = CapacityVector(CapacityValue::unknown(), CapacityValue::unknown(),
                                       CapacityValue(100));
  rack.uplink = ConnectivityState::UP;
  rack.evidence = declaration.evidence;
  declaration.racks.push_back(std::move(rack));
  return declaration;
}

}  // namespace

SF_TEST(failure_domains, nesting_resolves_depth_and_ancestry) {
  std::vector<MemberDomainDeclaration> declarations;
  declarations.push_back(domain_with_domain(
      "a", "fd-room", "", true, {ResourceKey::rack("rack-a")}, 1000000));
  declarations.push_back(domain_with_domain(
      "b", "fd-hall", "fd-room", false, {ResourceKey::rack("rack-b")}, 1000000));
  declarations.push_back(domain_with_domain(
      "c", "fd-site", "fd-hall", false, {ResourceKey::rack("rack-c")}, 1000000));
  for (auto& declaration : declarations) {
    SF_REQUIRE(is_ok(declaration.seal()));
  }

  SiteComposer composer;
  ComposedSite site;
  SF_REQUIRE(is_ok(composer.compose(sftest::input_over(declarations, 1000000), site)));
  SF_CHECK_EQ(Status::OK, site.failure_domains.status);
  SF_CHECK(site.failure_domains.cyclic.empty());
  SF_CHECK(site.failure_domains.dangling.empty());
  SF_CHECK_EQ(std::size_t(3), site.failure_domains.nodes.size());

  const FailureDomainNode* room = site.failure_domains.find(FailureDomainId::unchecked("fd-room"));
  const FailureDomainNode* hall = site.failure_domains.find(FailureDomainId::unchecked("fd-hall"));
  const FailureDomainNode* planet = site.failure_domains.find(FailureDomainId::unchecked("fd-site"));
  SF_REQUIRE(room != nullptr && hall != nullptr && planet != nullptr);
  SF_CHECK_EQ(std::uint32_t(0), room->depth);
  SF_CHECK_EQ(std::uint32_t(1), hall->depth);
  SF_CHECK_EQ(std::uint32_t(2), planet->depth);
  SF_CHECK(planet->ancestor_of(FailureDomainId::unchecked("fd-hall")));
  SF_CHECK(planet->ancestor_of(FailureDomainId::unchecked("fd-room")));
  SF_CHECK(!room->ancestor_of(FailureDomainId::unchecked("fd-hall")));
  SF_CHECK_EQ(std::size_t(1), room->children.size());
  SF_CHECK_EQ(FailureDomainClass::PHYSICAL, room->domain_class);
  SF_CHECK_EQ(FailureDomainClass::ADMINISTRATIVE, hall->domain_class);
}

SF_TEST(failure_domains, cycle_is_reported_and_not_resolved) {
  std::vector<MemberDomainDeclaration> declarations;
  declarations.push_back(domain_with_domain(
      "a", "fd-one", "fd-two", true, {ResourceKey::rack("rack-a")}, 1000000));
  declarations.push_back(domain_with_domain(
      "b", "fd-two", "fd-one", true, {ResourceKey::rack("rack-b")}, 1000000));
  for (auto& declaration : declarations) {
    SF_REQUIRE(is_ok(declaration.seal()));
  }
  SiteComposer composer;
  ComposedSite site;
  SF_REQUIRE(is_ok(composer.compose(sftest::input_over(declarations, 1000000), site)));
  SF_CHECK_EQ(Status::FAILURE_DOMAIN_CYCLE, site.failure_domains.status);
  SF_CHECK_EQ(std::size_t(2), site.failure_domains.cyclic.size());
  SF_CHECK_EQ(SiteLifecycle::CONFLICTING, site.lifecycle);
  for (const auto& node : site.failure_domains.nodes) {
    SF_CHECK_EQ(Status::FAILURE_DOMAIN_CYCLE, node.status);
    SF_CHECK(node.ancestors.empty());
  }
}

SF_TEST(failure_domains, dangling_parent_is_reported) {
  std::vector<MemberDomainDeclaration> declarations;
  declarations.push_back(domain_with_domain(
      "a", "fd-leaf", "fd-missing", true, {ResourceKey::rack("rack-a")}, 1000000));
  SF_REQUIRE(is_ok(declarations[0].seal()));
  SiteComposer composer;
  ComposedSite site;
  SF_REQUIRE(is_ok(composer.compose(sftest::input_over(declarations, 1000000), site)));
  SF_CHECK_EQ(Status::FAILURE_DOMAIN_DANGLING_PARENT, site.failure_domains.status);
  SF_CHECK_EQ(std::size_t(1), site.failure_domains.dangling.size());
  SF_CHECK_EQ(SiteLifecycle::INDETERMINATE, site.lifecycle);
}

SF_TEST(failure_domains, class_disagreement_is_a_conflict) {
  std::vector<MemberDomainDeclaration> declarations;
  declarations.push_back(domain_with_domain(
      "a", "fd-shared", "", true, {ResourceKey::rack("rack-a")}, 1000000));
  declarations.push_back(domain_with_domain(
      "b", "fd-shared", "", false, {ResourceKey::rack("rack-b")}, 1000000));
  for (auto& declaration : declarations) {
    SF_REQUIRE(is_ok(declaration.seal()));
  }
  SiteComposer composer;
  ComposedSite site;
  SF_REQUIRE(is_ok(composer.compose(sftest::input_over(declarations, 1000000), site)));
  SF_CHECK_EQ(SiteLifecycle::CONFLICTING, site.lifecycle);
  const FailureDomainNode* node =
      site.failure_domains.find(FailureDomainId::unchecked("fd-shared"));
  SF_REQUIRE(node != nullptr);
  SF_CHECK_EQ(Status::FAILURE_DOMAIN_CONFLICT, node->status);
}

SF_TEST(failure_domains, members_are_unioned_across_attestors) {
  std::vector<MemberDomainDeclaration> declarations;
  declarations.push_back(domain_with_domain(
      "a", "fd-shared", "", true, {ResourceKey::rack("rack-a")}, 1000000));
  declarations.push_back(domain_with_domain(
      "b", "fd-shared", "", true, {ResourceKey::rack("rack-a"), ResourceKey::rack("rack-b")},
      1000000));
  for (auto& declaration : declarations) {
    SF_REQUIRE(is_ok(declaration.seal()));
  }
  SiteComposer composer;
  ComposedSite site;
  SF_REQUIRE(is_ok(composer.compose(sftest::input_over(declarations, 1000000), site)));
  const FailureDomainNode* node =
      site.failure_domains.find(FailureDomainId::unchecked("fd-shared"));
  SF_REQUIRE(node != nullptr);
  SF_CHECK_EQ(std::size_t(2), node->members.size());
}

SF_TEST(failure_domains, separateness_must_be_proven) {
  std::vector<MemberDomainDeclaration> declarations;
  declarations.push_back(domain_with_domain(
      "a", "fd-one", "", true, {ResourceKey::rack("rack-a")}, 1000000));
  declarations.push_back(domain_with_domain(
      "b", "fd-two", "", true, {ResourceKey::rack("rack-b")}, 1000000));
  for (auto& declaration : declarations) {
    SF_REQUIRE(is_ok(declaration.seal()));
  }
  SiteComposer composer;
  ComposedSite site;
  SF_REQUIRE(is_ok(composer.compose(sftest::input_over(declarations, 1000000), site)));
  SF_CHECK(site.failure_domains.are_separate(ResourceKey::rack("rack-a"),
                                             ResourceKey::rack("rack-b")));
  SF_CHECK(!site.failure_domains.are_separate(ResourceKey::rack("rack-a"),
                                              ResourceKey::rack("rack-unknown")));
}

SF_TEST(failure_domains, deep_ancestry_is_bounded) {
  std::vector<MemberDomainDeclaration> declarations;
  const std::size_t depth = limits::kMaxFailureDomainDepth + 4;
  for (std::size_t index = 0; index < depth; ++index) {
    const std::string id = "fd-" + std::to_string(index);
    const std::string parent = index == 0 ? std::string() : "fd-" + std::to_string(index - 1);
    declarations.push_back(domain_with_domain("d" + std::to_string(index), id, parent, true,
                                              {ResourceKey::rack("rack-d" + std::to_string(index))},
                                              1000000));
  }
  for (auto& declaration : declarations) {
    SF_REQUIRE(is_ok(declaration.seal()));
  }
  SiteComposer composer;
  ComposedSite site;
  SF_REQUIRE(is_ok(composer.compose(sftest::input_over(declarations, 1000000), site)));
  for (const auto& node : site.failure_domains.nodes) {
    SF_CHECK(node.depth <= limits::kMaxFailureDomainDepth);
    SF_CHECK(node.ancestors.size() <= limits::kMaxFailureDomainDepth);
  }
}

SF_TEST(failure_domains, survivability_obligation_counts_populated_descendants) {
  std::vector<MemberDomainDeclaration> declarations;
  declarations.push_back(domain_with_domain(
      "a", "fd-root", "", true, {ResourceKey::rack("rack-a")}, 1000000));
  declarations.push_back(domain_with_domain(
      "b", "fd-left", "fd-root", true, {ResourceKey::rack("rack-b")}, 1000000));
  declarations.push_back(domain_with_domain(
      "c", "fd-right", "fd-root", true, {ResourceKey::rack("rack-c")}, 1000000));

  MemberDomainDeclaration obligation_domain;
  obligation_domain.domain = MemberDomainKey::shared_resource("domains");
  obligation_domain.site = SiteId::unchecked("site-alpha");
  obligation_domain.generation = Generation::initial();
  obligation_domain.observed_epoch = Epoch::initial();
  obligation_domain.evidence = Evidence::reported(1000000, 60000);
  ProtectedObligation obligation;
  obligation.id = ObligationId::unchecked("survive");
  obligation.kind = ObligationKind::DOMAIN_SURVIVABILITY;
  obligation.scope_domain = FailureDomainId::unchecked("fd-root");
  obligation.required = 2;
  obligation.generation = Generation::initial();
  obligation.evidence = obligation_domain.evidence;
  obligation_domain.obligations.push_back(std::move(obligation));
  declarations.push_back(std::move(obligation_domain));

  for (auto& declaration : declarations) {
    SF_REQUIRE(is_ok(declaration.seal()));
  }

  SiteComposer composer;
  ComposedSite site;
  SF_REQUIRE(is_ok(composer.compose(sftest::input_over(declarations, 1000000), site)));
  SF_REQUIRE(!site.obligations.empty());
  SF_CHECK_EQ(Status::OK, site.obligations.front().status);
  SF_CHECK_EQ(std::uint64_t(2), site.obligations.front().observed);
}

SF_TEST(failure_domains, distinct_failure_domain_obligation_is_measured) {
  std::vector<MemberDomainDeclaration> declarations;
  declarations.push_back(domain_with_domain(
      "a", "fd-one", "", true, {ResourceKey::rack("rack-a")}, 1000000));
  declarations.push_back(domain_with_domain(
      "b", "fd-two", "", true, {ResourceKey::rack("rack-b")}, 1000000));

  MemberDomainDeclaration obligation_domain;
  obligation_domain.domain = MemberDomainKey::shared_resource("domains");
  obligation_domain.site = SiteId::unchecked("site-alpha");
  obligation_domain.generation = Generation::initial();
  obligation_domain.observed_epoch = Epoch::initial();
  obligation_domain.evidence = Evidence::reported(1000000, 60000);
  ProtectedObligation obligation;
  obligation.id = ObligationId::unchecked("diverse");
  obligation.kind = ObligationKind::MIN_DISTINCT_FAILURE_DOMAINS;
  obligation.required = 2;
  obligation.generation = Generation::initial();
  obligation.evidence = obligation_domain.evidence;
  obligation_domain.obligations.push_back(std::move(obligation));
  declarations.push_back(std::move(obligation_domain));

  for (auto& declaration : declarations) {
    SF_REQUIRE(is_ok(declaration.seal()));
  }

  SiteComposer composer;
  ComposedSite site;
  SF_REQUIRE(is_ok(composer.compose(sftest::input_over(declarations, 1000000), site)));
  SF_REQUIRE(!site.obligations.empty());
  SF_CHECK_EQ(Status::OK, site.obligations.front().status);
  SF_CHECK_EQ(std::uint64_t(2), site.obligations.front().observed);
}
